
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/notifications_manager_linux.h"

#include "base/platform/base_platform_info.h"
#include "platform/platform_specific.h"
#include "core/sandbox.h"
#include "data/data_forum_topic.h"
#include "data/data_saved_sublist.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "lang/lang_keys.h"
#include "window/notifications_utilities.h"

#include <QtCore/QBuffer>

#include <ksandbox.h>

#include <gio/gio.hpp>

#include <dlfcn.h>

namespace Platform {
namespace Notifications {
namespace {

using namespace gi::repository;

} // namespace

bool SkipToastForCustom() {
	return false;
}

void MaybePlaySoundForCustom(Fn<void()> playSound) {
	playSound();
}

void MaybeFlashBounceForCustom(Fn<void()> flashBounce) {
	flashBounce();
}

bool WaitForInputForCustom() {
	return true;
}

bool Supported() {
	return bool(Gio::Application::get_default());
}

bool Enforced() {
	// Wayland doesn't support positioning
	// and custom notifications don't work here
	return IsWayland();
}

bool ByDefault() {
	return false;
}

void Create(Window::Notifications::System *system) {
	system->setManager([=] { return std::make_unique<Manager>(system); });
}

class Manager::Private {
public:
	explicit Private(not_null<Manager*> manager);

	void showNotification(
		NotificationInfo &&info,
		Ui::PeerUserpicView &userpicView);
	void clearAll();
	void clearFromItem(not_null<HistoryItem*> item);
	void clearFromTopic(not_null<Data::ForumTopic*> topic);
	void clearFromSublist(not_null<Data::SavedSublist*> sublist);
	void clearFromHistory(not_null<History*> history);
	void clearFromSession(not_null<Main::Session*> session);

private:
	const not_null<Manager*> _manager;
	Gio::Application _application;
	base::flat_map<
		ContextId,
		base::flat_map<MsgId, rpl::lifetime>> _notifications;
	rpl::lifetime _lifetime;

};

Manager::Private::Private(not_null<Manager*> manager)
: _manager(manager)
, _application(Gio::Application::get_default()) {
	auto actionMap = Gio::ActionMap(_application);

	const auto dictToNotificationId = [](GLib::VariantDict dict) {
		return NotificationId{
			.contextId = ContextId{
				.sessionId = dict.lookup_value("session").get_uint64(),
				.peerId = PeerId(dict.lookup_value("peer").get_uint64()),
				.topicRootId = MsgId(dict.lookup_value("topic").get_int64()),
				.monoforumPeerId = PeerId(
					dict.lookup_value("monoforumpeer").get_uint64()),
			},
			.msgId = dict.lookup_value("msgid").get_int64(),
		};
	};

	auto activate = gi::wrap(
		G_SIMPLE_ACTION(
			actionMap.lookup_action("notification-activate").gobj_()),
		gi::transfer_none);

	const auto activateSignal = activate.signal_activate().connect([=](
			Gio::SimpleAction,
			GLib::Variant parameter) {
		Core::Sandbox::Instance().customEnterFromEventLoop([&] {
			_manager->notificationActivated(
				dictToNotificationId(GLib::VariantDict::new_(parameter)));
		});
	});

	_lifetime.add([=]() mutable {
		activate.disconnect(activateSignal);
	});

	auto markAsRead = gi::wrap(
		G_SIMPLE_ACTION(
			actionMap.lookup_action("notification-mark-as-read").gobj_()),
		gi::transfer_none);

	const auto markAsReadSignal = markAsRead.signal_activate().connect([=](
			Gio::SimpleAction,
			GLib::Variant parameter) {
		Core::Sandbox::Instance().customEnterFromEventLoop([&] {
			_manager->notificationReplied(
				dictToNotificationId(GLib::VariantDict::new_(parameter)),
				{});
		});
	});

	_lifetime.add([=]() mutable {
		markAsRead.disconnect(markAsReadSignal);
	});
}

void Manager::Private::showNotification(
		NotificationInfo &&info,
		Ui::PeerUserpicView &userpicView) {
	const auto peer = info.peer;
	const auto options = info.options;
	const auto key = ContextId{
		.sessionId = peer->session().uniqueId(),
		.peerId = peer->id,
		.topicRootId = info.topicRootId,
		.monoforumPeerId = info.monoforumPeerId,
	};

	auto notification = Gio::Notification::new_(info.title.toStdString());

	notification.set_body(info.subtitle.isEmpty()
		? info.message.toStdString()
		: tr::lng_dialogs_text_with_from(
			tr::now,
			lt_from_part,
			tr::lng_dialogs_text_from_wrapped(
				tr::now,
				lt_from,
				info.subtitle),
			lt_message,
			info.message).toStdString());

	notification.set_icon(
		Gio::ThemedIcon::new_(ApplicationIconName().toStdString()));

	// for chat messages, according to
	// https://docs.gtk.org/gio/enum.NotificationPriority.html
	notification.set_priority(Gio::NotificationPriority::HIGH_);

	// glib 2.70+, we keep glib 2.56+ compatibility
	static const auto set_category = [] {
		// reset dlerror after dlsym call
		const auto guard = gsl::finally([] { dlerror(); });
		return reinterpret_cast<void(*)(GNotification*, const gchar*)>(
			dlsym(RTLD_DEFAULT, "g_notification_set_category"));
	}();

	if (set_category) {
		set_category(notification.gobj_(), "im.received");
	}

	const auto notificationVariant = GLib::Variant::new_array({
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("session"),
			GLib::Variant::new_variant(
				GLib::Variant::new_uint64(peer->session().uniqueId()))),
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("peer"),
			GLib::Variant::new_variant(
				GLib::Variant::new_uint64(peer->id.value))),
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("peer"),
			GLib::Variant::new_variant(
				GLib::Variant::new_uint64(peer->id.value))),
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("topic"),
			GLib::Variant::new_variant(
				GLib::Variant::new_int64(info.topicRootId.bare))),
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("monoforumpeer"),
			GLib::Variant::new_variant(
				GLib::Variant::new_uint64(info.monoforumPeerId.value))),
		GLib::Variant::new_dict_entry(
			GLib::Variant::new_string("msgid"),
			GLib::Variant::new_variant(
				GLib::Variant::new_int64(info.itemId.bare))),
	});

	notification.set_default_action_and_target(
		"app.notification-activate",
		notificationVariant);

	if (!options.hideMarkAsRead) {
		notification.add_button_with_target(
			tr::lng_context_mark_read(tr::now).toStdString(),
			"app.notification-mark-as-read",
			notificationVariant);
	}

	if (!options.hideNameAndPhoto) {
		QByteArray imageData;
		QBuffer buffer(&imageData);
		buffer.open(QIODevice::WriteOnly);
		Window::Notifications::GenerateUserpic(peer, userpicView).save(
			&buffer,
			"PNG");

		notification.set_icon(
			Gio::BytesIcon::new_(
				GLib::Bytes::new_with_free_func(
					reinterpret_cast<const uchar*>(imageData.constData()),
					imageData.size(),
					[imageData] {})));
	}

	const auto id = Gio::dbus_generate_guid();
	_application.send_notification(id, notification);
	_notifications[key][info.itemId] = [=] {
		_application.withdraw_notification(id);
	};
}

void Manager::Private::clearAll() {
	_notifications.clear();
}

void Manager::Private::clearFromItem(not_null<HistoryItem*> item) {
	const auto i = _notifications.find(ContextId{
		.sessionId = item->history()->session().uniqueId(),
		.peerId = item->history()->peer->id,
		.topicRootId = item->topicRootId(),
		.monoforumPeerId = item->sublistPeerId(),
	});
	if (i != _notifications.cend()
			&& i->second.remove(item->id)
			&& i->second.empty()) {
		_notifications.erase(i);
	}
}

void Manager::Private::clearFromTopic(not_null<Data::ForumTopic*> topic) {
	_notifications.remove(ContextId{
		.sessionId = topic->session().uniqueId(),
		.peerId = topic->history()->peer->id,
		.topicRootId = topic->rootId(),
	});
}

void Manager::Private::clearFromSublist(
		not_null<Data::SavedSublist*> sublist) {
	_notifications.remove(ContextId{
		.sessionId = sublist->session().uniqueId(),
		.peerId = sublist->owningHistory()->peer->id,
		.monoforumPeerId = sublist->sublistPeer()->id,
	});
}

void Manager::Private::clearFromHistory(not_null<History*> history) {
	const auto sessionId = history->session().uniqueId();
	const auto peerId = history->peer->id;
	auto i = _notifications.lower_bound(ContextId{
		.sessionId = sessionId,
		.peerId = peerId,
	});
	while (i != _notifications.cend()
		&& i->first.sessionId == sessionId
		&& i->first.peerId == peerId) {
		i = _notifications.erase(i);
	}
}

void Manager::Private::clearFromSession(not_null<Main::Session*> session) {
	const auto sessionId = session->uniqueId();
	auto i = _notifications.lower_bound(ContextId{
		.sessionId = sessionId,
	});
	while (i != _notifications.cend() && i->first.sessionId == sessionId) {
		i = _notifications.erase(i);
	}
}

Manager::Manager(not_null<Window::Notifications::System*> system)
: NativeManager(system)
, _private(std::make_unique<Private>(this)) {
}

Manager::~Manager() = default;

void Manager::doShowNativeNotification(
		NotificationInfo &&info,
		Ui::PeerUserpicView &userpicView) {
	_private->showNotification(std::move(info), userpicView);
}

void Manager::doClearAllFast() {
	_private->clearAll();
}

void Manager::doClearFromItem(not_null<HistoryItem*> item) {
	_private->clearFromItem(item);
}

void Manager::doClearFromTopic(not_null<Data::ForumTopic*> topic) {
	_private->clearFromTopic(topic);
}

void Manager::doClearFromSublist(not_null<Data::SavedSublist*> sublist) {
	_private->clearFromSublist(sublist);
}

void Manager::doClearFromHistory(not_null<History*> history) {
	_private->clearFromHistory(history);
}

void Manager::doClearFromSession(not_null<Main::Session*> session) {
	_private->clearFromSession(session);
}

bool Manager::doSkipToast() const {
	return false;
}

void Manager::doMaybePlaySound(Fn<void()> playSound) {
	playSound();
}

void Manager::doMaybeFlashBounce(Fn<void()> flashBounce) {
	flashBounce();
}

} // namespace Notifications
} // namespace Platform
