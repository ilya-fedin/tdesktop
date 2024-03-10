
/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/notifications_manager_linux.h"

#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_forum_topic.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "lang/lang_keys.h"
#include "window/notifications_utilities.h"

#include <QtCore/QBuffer>

#include <glibmm.h>
#include <gio/gio.hpp>

#include <dlfcn.h>

using namespace gi::repository;

namespace Platform {
namespace Notifications {

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
	if ((Core::App().settings().nativeNotifications() || Enforced())
			&& Supported()) {
		system->setManager(std::make_unique<Manager>(system));
	} else if (Enforced()) {
		using DummyManager = Window::Notifications::DummyManager;
		system->setManager(std::make_unique<DummyManager>(system));
	} else {
		system->setManager(nullptr);
	}
}

class Manager::Private {
public:
	explicit Private(not_null<Manager*> manager);

	void showNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options);
	void clearAll();
	void clearFromItem(not_null<HistoryItem*> item);
	void clearFromTopic(not_null<Data::ForumTopic*> topic);
	void clearFromHistory(not_null<History*> history);
	void clearFromSession(not_null<Main::Session*> session);
	void clearNotification(NotificationId id);

	~Private();

private:
	const not_null<Manager*> _manager;
	Gio::Application _application;

	base::flat_map<
		ContextId,
		base::flat_map<MsgId, std::string>> _notifications;

};

Manager::Private::Private(not_null<Manager*> manager)
: _manager(manager)
, _application(Gio::Application::get_default()) {
}

void Manager::Private::showNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) {
	if (!_application) {
		return;
	}

	const auto key = ContextId{
		.sessionId = peer->session().uniqueId(),
		.peerId = peer->id,
		.topicRootId = topicRootId,
	};

	const auto notificationId = NotificationId{
		.contextId = key,
		.msgId = msgId,
	};

	auto notification = Gio::Notification::new_(
		subtitle.isEmpty()
			? title.toStdString()
			: subtitle.toStdString() + " (" + title.toStdString() + ')');

	notification.set_body(msg.toStdString());

	notification.set_icon(
		Gio::ThemedIcon::new_(base::IconName().toStdString()));

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

	const auto notificationIdVariant = gi::wrap(
		Glib::create_variant(notificationId.toTuple()).gobj_copy(),
		gi::transfer_full);

	notification.set_default_action_and_target(
		"app.notification-activate",
		notificationIdVariant);

	if (!options.hideMarkAsRead) {
		notification.add_button_with_target(
			tr::lng_context_mark_read(tr::now).toStdString(),
			"app.notification-mark-as-read",
			notificationIdVariant);
	}

	if (!options.hideNameAndPhoto) {
		using DestroyNotify = gi::detail::callback<
			void(),
			gi::transfer_full_t,
			std::tuple<>
		>;

		const auto imageData = std::make_shared<QByteArray>();
		QBuffer buffer(imageData.get());
		buffer.open(QIODevice::WriteOnly);
		Window::Notifications::GenerateUserpic(peer, userpicView).save(
			&buffer,
			"PNG");

		const auto callbackWrap = gi::unwrap(
			DestroyNotify([imageData] {}),
			gi::scope_notified);

		notification.set_icon(
			Gio::BytesIcon::new_(
				gi::wrap(g_bytes_new_with_free_func(
					imageData->constData(),
					imageData->size(),
					&callbackWrap->destroy,
					callbackWrap), gi::transfer_full)));
	}

	auto i = _notifications.find(key);
	if (i != end(_notifications)) {
		auto j = i->second.find(msgId);
		if (j != end(i->second)) {
			auto oldNotification = std::move(j->second);
			i->second.erase(j);
			_application.withdraw_notification(oldNotification);
			clearNotification(notificationId);
			i = _notifications.find(key);
		}
	}

	if (i == end(_notifications)) {
		i = _notifications.emplace(
			key,
			base::flat_map<MsgId, std::string>()).first;
	}

	const auto j = i->second.emplace(
		msgId,
		Gio::dbus_generate_guid()).first;

	_application.send_notification(j->second, notification);
}

void Manager::Private::clearAll() {
	for (const auto &[key, notifications] : base::take(_notifications)) {
		for (const auto &[msgId, notification] : notifications) {
			const auto notificationId = NotificationId{
				.contextId = key,
				.msgId = msgId,
			};
			_application.withdraw_notification(notification);
			clearNotification(notificationId);
		}
	}
}

void Manager::Private::clearFromItem(not_null<HistoryItem*> item) {
	const auto key = ContextId{
		.sessionId = item->history()->session().uniqueId(),
		.peerId = item->history()->peer->id,
		.topicRootId = item->topicRootId(),
	};
	const auto notificationId = NotificationId{
		.contextId = key,
		.msgId = item->id,
	};
	const auto i = _notifications.find(key);
	if (i == _notifications.cend()) {
		return;
	}
	const auto j = i->second.find(item->id);
	if (j == i->second.end()) {
		return;
	}
	const auto taken = base::take(j->second);
	i->second.erase(j);
	if (i->second.empty()) {
		_notifications.erase(i);
	}
	_application.withdraw_notification(taken);
	clearNotification(notificationId);
}

void Manager::Private::clearFromTopic(not_null<Data::ForumTopic*> topic) {
	const auto key = ContextId{
		.sessionId = topic->session().uniqueId(),
		.peerId = topic->history()->peer->id
	};
	const auto i = _notifications.find(key);
	if (i != _notifications.cend()) {
		const auto temp = base::take(i->second);
		_notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			const auto notificationId = NotificationId{
				.contextId = key,
				.msgId = msgId,
			};
			_application.withdraw_notification(notification);
			clearNotification(notificationId);
		}
	}
}

void Manager::Private::clearFromHistory(not_null<History*> history) {
	const auto sessionId = history->session().uniqueId();
	const auto peerId = history->peer->id;
	const auto key = ContextId{
		.sessionId = sessionId,
		.peerId = peerId,
	};
	auto i = _notifications.lower_bound(key);
	while (i != _notifications.cend()
		&& i->first.sessionId == sessionId
		&& i->first.peerId == peerId) {
		const auto temp = base::take(i->second);
		i = _notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			const auto notificationId = NotificationId{
				.contextId = key,
				.msgId = msgId,
			};
			_application.withdraw_notification(notification);
			clearNotification(notificationId);
		}
	}
}

void Manager::Private::clearFromSession(not_null<Main::Session*> session) {
	const auto sessionId = session->uniqueId();
	const auto key = ContextId{
		.sessionId = sessionId,
	};
	auto i = _notifications.lower_bound(key);
	while (i != _notifications.cend() && i->first.sessionId == sessionId) {
		const auto temp = base::take(i->second);
		i = _notifications.erase(i);

		for (const auto &[msgId, notification] : temp) {
			const auto notificationId = NotificationId{
				.contextId = key,
				.msgId = msgId,
			};
			_application.withdraw_notification(notification);
			clearNotification(notificationId);
		}
	}
}

void Manager::Private::clearNotification(NotificationId id) {
	auto i = _notifications.find(id.contextId);
	if (i != _notifications.cend()) {
		if (i->second.remove(id.msgId) && i->second.empty()) {
			_notifications.erase(i);
		}
	}
}

Manager::Private::~Private() {
	clearAll();
}

Manager::Manager(not_null<Window::Notifications::System*> system)
: NativeManager(system)
, _private(std::make_unique<Private>(this)) {
}

Manager::~Manager() = default;

void Manager::doShowNativeNotification(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		Ui::PeerUserpicView &userpicView,
		MsgId msgId,
		const QString &title,
		const QString &subtitle,
		const QString &msg,
		DisplayOptions options) {
	_private->showNotification(
		peer,
		topicRootId,
		userpicView,
		msgId,
		title,
		subtitle,
		msg,
		options);
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
