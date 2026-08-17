// This file is part of Telegram Desktop,
// the official desktop application for the Telegram messaging service.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
//
// Benchmark for Ui::Text::String, driven only through its public API, so that
// the same binary can be rebuilt against a reworked text engine and compared.
//
// Stages:
//   layout - setText / setMarkedText, i.e. parse + WordParser
//   wrap   - countHeight, i.e. line enumeration at a given width
//   paint  - draw into a QImage
//
#include "base/assertion.h"
#include "base/basic_types.h"
#include "base/flat_map.h"

#include "base/integration.h"
#include "ui/basic_click_handlers.h"
#include "ui/emoji_config.h"
#include "ui/text/text.h"
#include "ui/text/text_entity.h"
#include "ui/text/text_custom_emoji.h"
#include "ui/integration.h"
#include "ui/effects/animations.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_direction.h"
#include "ui/style/style_core_font.h"
#include "styles/style_basic.h"

// This file is built against several versions of the library, and they differ
// in what they ask of it, so each is recognized by a header that comes with it:
// the port over the public Qt API brings text_script_analysis.h, and the one
// behind the shaping interface brings text_shaper.h and nothing else.
#if __has_include("ui/text/text_script_analysis.h")
#include "ui/text/text_shaper.h"

// A shaping cache hides exactly what the cold measurement is after, so it has
// to be dropped before every one of them - and only that port keeps one.
#define TEXT_BENCH_HAS_SHAPING_CACHE
#elif __has_include("ui/text/text_shaper.h")

// Fonts are written out for fontconfig there, and where they go is asked of us.
#define TEXT_BENCH_HAS_FONTS_CACHE_FOLDER
#endif // __has_include("ui/text/text_shaper.h")

#include <rpl/rpl.h>

#include <functional>

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextBoundaryFinder>
#include <QtCore/QTemporaryDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtGui/QFontMetrics>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtGui/QTextLayout>
#include <QtGui/QTextObject>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <malloc.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

class BaseIntegration final : public base::Integration {
public:
	using base::Integration::Integration;

	void enterFromEventLoop(FnMut<void()> &&method) override {
		method();
	}
	bool logSkipDebug() override {
		return true;
	}
	void logMessageDebug(const QString &message) override {
	}
	void logMessage(const QString &message) override {
		std::fprintf(stderr, "%s\n", message.toUtf8().constData());
	}
};

class Integration final : public Ui::Integration {
public:
	void postponeCall(FnMut<void()> &&callable) override {
		callable();
	}
	void registerLeaveSubscription(not_null<QWidget*> widget) override {
	}
	void unregisterLeaveSubscription(not_null<QWidget*> widget) override {
	}
	QString emojiCacheFolder() override {
		return _dir.path() + u"/emoji"_q;
	}
#ifdef TEXT_BENCH_HAS_FONTS_CACHE_FOLDER
	QString fontsCacheFolder() override {
		return _dir.path() + u"/fonts"_q;
	}
#endif // TEXT_BENCH_HAS_FONTS_CACHE_FOLDER
	QString openglCheckFilePath() override {
		return _dir.path() + u"/opengl"_q;
	}
	QString angleBackendFilePath() override {
		return _dir.path() + u"/angle"_q;
	}
	void touchCounterIncrement() override {
	}
	int touchCounterNow() override {
		return 0;
	}

private:
	QTemporaryDir _dir;

};

struct Sample {
	std::string name;
	Ui::Text::String text;
	QString source;
	int width = 0;
};

// Bytes currently handed out by malloc. Covers both paths: QTextEngine grows
// its glyph pool with realloc, QList uses operator new, both land here.
[[nodiscard]] int64 HeapInUse() {
#if defined __GLIBC__ && defined __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 33)
	return int64(mallinfo2().uordblks);
#else // __GLIBC_PREREQ(2, 33)
	return int64(mallinfo().uordblks);
#endif // !__GLIBC_PREREQ(2, 33)
#else // __GLIBC__ && __GLIBC_PREREQ
	return int64(mallinfo().uordblks);
#endif // !__GLIBC__ || !__GLIBC_PREREQ
}

[[nodiscard]] int64 ReadProcStatus(const char *field) {
	auto file = QFile(u"/proc/self/status"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return 0;
	}
	const auto lines = QString::fromLatin1(file.readAll()).split('\n');
	for (const auto &line : lines) {
		if (line.startsWith(QLatin1String(field))) {
			const auto parts = line.split(
				QRegularExpression(u"\\s+"_q),
				Qt::SkipEmptyParts);
			return (parts.size() > 1) ? parts[1].toLongLong() : 0;
		}
	}
	return 0;
}

[[nodiscard]] int64 PeakRssKb() {
	return ReadProcStatus("VmHWM:");
}

[[nodiscard]] int64 RssKb() {
	return ReadProcStatus("VmRSS:");
}

void DropShapingCache() {
#ifdef TEXT_BENCH_HAS_SHAPING_CACHE
	Ui::Text::Shaper::ClearCache();
#endif // TEXT_BENCH_HAS_SHAPING_CACHE
}

template <typename Callback>
[[nodiscard]] double Measure(int iterations, Callback &&callback) {
	callback(); // warm up
	const auto start = std::chrono::steady_clock::now();
	for (auto i = 0; i != iterations; ++i) {
		callback();
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	const auto ns = std::chrono::duration<double, std::nano>(elapsed).count();
	return iterations ? (ns / iterations) : 0.;
}

// The samples below are written to reach the corners of the engine, and a real
// history reaches none of them on purpose: it is what a screenful of messages
// actually costs. Taken from an export of one, so that the mix of scripts, of
// lengths and of emoji is somebody's real one and not a guess.
// What an export calls a piece of formatting, in the names of the engine. The
// ones with no entity of their own - a plain piece, a link that is only itself
// - are left out: the parser finds those in the text anyway.
[[nodiscard]] EntityType EntityTypeByName(const QString &name) {
	static const auto kTypes = base::flat_map<QString, EntityType>{
		{ u"bold"_q, EntityType::Bold },
		{ u"italic"_q, EntityType::Italic },
		{ u"underline"_q, EntityType::Underline },
		{ u"strikethrough"_q, EntityType::StrikeOut },
		{ u"code"_q, EntityType::Code },
		{ u"pre"_q, EntityType::Pre },
		{ u"blockquote"_q, EntityType::Blockquote },
		{ u"spoiler"_q, EntityType::Spoiler },
		{ u"text_link"_q, EntityType::CustomUrl },
		{ u"link"_q, EntityType::Url },
		{ u"email"_q, EntityType::Email },
		{ u"phone"_q, EntityType::Phone },
		{ u"mention"_q, EntityType::Mention },
		{ u"mention_name"_q, EntityType::MentionName },
		{ u"hashtag"_q, EntityType::Hashtag },
		{ u"cashtag"_q, EntityType::Cashtag },
		{ u"bank_card"_q, EntityType::BankCard },
		{ u"bot_command"_q, EntityType::BotCommand },
	};
	const auto i = kTypes.find(name);
	return (i == end(kTypes)) ? EntityType::Invalid : i->second;
}

[[nodiscard]] std::vector<TextWithEntities> HistoryMessages(
		const QString &path,
		int count) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	const auto messages = document.object().value(u"messages"_q).toArray();
	auto result = std::vector<TextWithEntities>();
	for (const auto &value : messages) {
		const auto message = value.toObject();
		if (message.value(u"type"_q).toString() != u"message"_q) {
			continue;
		}
		// The text of a message is a string when it carries no formatting and
		// a list of pieces when it does, each of them a string or an object
		// with the text in it - a link, a mention, a piece of code. An export
		// made by hand may carry only the pieces, so both are read.
		const auto join = [](const QJsonValue &value) {
			auto result = TextWithEntities();
			if (value.isString()) {
				result.text = value.toString();
				return result;
			}
			for (const auto &piece : value.toArray()) {
				if (piece.isString()) {
					result.text += piece.toString();
					continue;
				}
				const auto object = piece.toObject();
				const auto text = object.value(u"text"_q).toString();
				const auto type = EntityTypeByName(
					object.value(u"type"_q).toString());
				if (type != EntityType::Invalid) {
					result.entities.push_back({
						type,
						int(result.text.size()),
						int(text.size()),
						object.value(u"href"_q).toString(),
					});
				}
				result.text += text;
			}
			return result;
		};
		auto joined = join(message.value(u"text"_q));
		if (joined.text.isEmpty()) {
			joined = join(message.value(u"text_entities"_q));
		}
		if (joined.text.isEmpty()) {
			continue;
		}
		result.push_back(std::move(joined));
	}

	// The last of them, which is what opening a chat shows.
	if (int(result.size()) > count) {
		result.erase(begin(result), end(result) - count);
	}
	return result;
}

// What opening a chat costs, which is the number the samples are there to
// explain: a screenful of real messages, laid out and drawn as a batch.
void MeasureHistory(
		const QString &path,
		const style::TextStyle &st,
		int width,
		int iterations,
		bool rich,
		bool layoutOnly,
		bool paintOnly) {
	constexpr auto kMessages = 40;

	const auto messages = HistoryMessages(path, kMessages);
	if (messages.empty()) {
		std::printf("history %s: no messages found\n",
			path.toUtf8().constData());
		return;
	}
	auto characters = 0;
	auto entities = 0;
	for (const auto &message : messages) {
		characters += int(message.text.size());
		entities += int(message.entities.size());
	}

	auto texts = std::vector<Ui::Text::String>();
	texts.reserve(messages.size());
	for (auto i = 0, count = int(messages.size()); i != count; ++i) {
		texts.emplace_back(32);
	}
	// With the formatting the export carries, or with the text of it alone -
	// the plain way is what every number before this was taken with, and the
	// marked one is what a real message costs, where a link or a piece of bold
	// makes a block of its own.
	const auto assign = rich
		? std::function<void()>([&] {
			for (auto i = 0, count = int(messages.size()); i != count; ++i) {
				texts[i].setMarkedText(st, messages[i]);
			}
		})
		: std::function<void()>([&] {
			for (auto i = 0, count = int(messages.size()); i != count; ++i) {
				texts[i].setText(st, messages[i].text);
			}
		});

	// The whole batch at once, the way a chat is opened: nothing of it was
	// shaped before, so no cache of a previous message can help the next one.
	auto cold = 0.;
	for (auto i = 0; i != (paintOnly ? 1 : iterations); ++i) {
		DropShapingCache();
		const auto start = std::chrono::steady_clock::now();
		assign();
		const auto elapsed = std::chrono::steady_clock::now() - start;
		cold += std::chrono::duration<double, std::nano>(elapsed).count();
	}
	cold /= (paintOnly ? 1 : iterations);

	auto height = 0;
	for (auto &text : texts) {
		height += text.countHeight(width);
	}

	auto image = QImage(
		width * 2,
		2048,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(1.);
	image.fill(Qt::transparent);
	auto painter = QPainter(&image);
	painter.setPen(Qt::black);
	auto context = Ui::Text::PaintContext();
	context.outerWidth = width;
	context.availableWidth = width;
	context.palette = &st::defaultTextPalette;
	const auto paint = Measure(layoutOnly ? 0 : iterations, [&] {
		auto top = 0;
		for (auto &text : texts) {
			context.position = QPoint(0, top % 1024);
			text.draw(painter, context);
			top += st.font->height;
		}
	});
	painter.end();

	std::printf("history: %d messages, %d characters, %d entities, height %d\n",
		int(messages.size()),
		characters,
		entities,
		height);
	std::printf("layout %.2f ms | paint %.2f ms\n",
		cold / 1e6,
		paint / 1e6);
}

[[nodiscard]] QString Latin() {
	return u"The quick brown fox jumps over the lazy dog while the other "
		"dogs watch in silence and wonder what is going on in this "
		"particular corner of the world today, honestly speaking."_q;
}

[[nodiscard]] QString Cyrillic() {
	return u"Съешь же ещё этих мягких французских булок да выпей чаю, "
		"а потом расскажи всем остальным, что именно произошло этим "
		"утром в этом тихом городе, и почему это оказалось важно."_q;
}

[[nodiscard]] QString Arabic() {
	return u"النص العربي هنا يحتوي على حروف متصلة وكلمات كثيرة تكفي "
		"لقياس أداء التشكيل والرسم في هذه التجربة البسيطة جدا "
		"والتي تهدف إلى مقارنة المسارين بشكل عادل تماما اليوم."_q;
}

[[nodiscard]] QString Devanagari() {
	return u"यह हिन्दी का एक उदाहरण वाक्य है जिसमें पर्याप्त अक्षर हैं "
		"ताकि आकार देने और चित्रण की गति को ठीक से मापा जा सके "
		"और दोनों रास्तों की तुलना निष्पक्ष रूप से की जा सके।"_q;
}

// A label of the size a bot button carries, where the width the engine reports
// is what centers it - and this one, the dependent vowel signs of Bengali with
// no consonant to sit on, is where that width was seen to disagree with what
// ends up drawn: a mark carries no advance of its own and ink all the same.
[[nodiscard]] QString Bengali() {
	return u"া, ি, ী, ু, ূ, "
		"ে, ৈ, ো, ৌ"_q;
}

constexpr auto kEmojiSide = 20;

// About what a timestamp with two ticks takes in a bubble.
constexpr auto kSkipBlockWidth = 46;

[[nodiscard]] const QImage &SharedEmojiImage() {
	static const auto result = [] {
		auto image = QImage(
			kEmojiSide,
			kEmojiSide,
			QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::magenta);
		return image;
	}();
	return result;
}

// A custom emoji that costs what the text engine makes it cost and nothing
// more: one shared image for everybody, so the per-emoji figure measures the
// blocks and objects the engine creates, not a frame cache.
class BenchCustomEmoji final : public Ui::Text::CustomEmoji {
public:
	static constexpr auto kSide = kEmojiSide;

	explicit BenchCustomEmoji(QString data) : _data(std::move(data)) {
	}

	int width() override {
		return kSide;
	}
	QString entityData() override {
		return _data;
	}
	void paint(QPainter &p, const Context &context) override {
		p.drawImage(context.position, SharedEmojiImage());
	}
	void unload() override {
	}
	bool ready() override {
		return true;
	}
	bool readyInDefaultState() override {
		return true;
	}

private:
	QString _data;

};

// `count` custom emoji, optionally with a word between each pair - the
// interleaved shape is what splits a message into many object-free stretches.
[[nodiscard]] TextWithEntities CustomEmojiText(int count, bool withWords) {
	auto result = TextWithEntities();
	const auto word = u"word "_q;
	for (auto i = 0; i != count; ++i) {
		if (withWords) {
			result.text += word;
		}
		result.entities.push_back({
			EntityType::CustomEmoji,
			int(result.text.size()),
			1,
			u"bench:%1"_q.arg(i % 16),
		});
		result.text += QChar(0xE000 + (i % 16));
	}
	return result;
}

// Emoji that come with the application rather than through a factory: a
// different block type, a width taken from the emoji itself and a sprite to
// blit. Includes a variation-selected one and a skin tone, so the parser has to
// take several code points as one emoji. A space that follows an emoji goes into
// the emoji block itself, and itemization then splits that block into two items,
// the emoji and its spaces - which nothing else in the corpus reaches, hence
// spaceAfter and a sample of its own.
[[nodiscard]] QString NativeEmojiText(
		int count,
		bool withWords,
		bool spaceAfter = false) {
	const auto emoji = std::array{
		QString::fromUtf8("\xF0\x9F\x98\x80"), // U+1F600
		QString::fromUtf8("\xE2\x9D\xA4\xEF\xB8\x8F"), // U+2764 U+FE0F
		QString::fromUtf8("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"), // +tone
		QString::fromUtf8("\xF0\x9F\x8E\x89"), // U+1F389
	};
	auto result = QString();
	for (auto i = 0; i != count; ++i) {
		if (withWords) {
			result += u"word "_q;
		}
		result += emoji[i % emoji.size()];
		if (spaceAfter) {
			result += ' ';
		}
	}
	return result;
}

[[nodiscard]] Ui::Text::MarkedContext EmojiContext() {
	auto result = Ui::Text::MarkedContext();
	result.customEmojiFactory = [](
			QStringView data,
			const Ui::Text::MarkedContext &) {
		return std::make_unique<BenchCustomEmoji>(data.toString());
	};
	result.repaint = [] {};
	return result;
}

// Line breaking stress: the classes that UAX#14 treats differently from
// QChar::isSpace, plus the cases tdesktop special-cases itself.
[[nodiscard]] QString Spaces() {
	return u"word\u2002en word\u2003em word\u2009thin word\u00A0nbsp "
		"word\u200Bzwsp word\u00ADsoft\u00ADhyphen word\u3000ideographic "
		"tab\tseparated and a trailing run of spaces    here."_q;
}

constexpr auto kShotMargin = 16;

// An empty paragraph in the middle: a selection across it fills a line that has
// no items at all, which nothing else here does.
[[nodiscard]] QString Paragraphs() {
	return u"First paragraph of the text goes here.\n\n"
		"Second paragraph comes after an empty line.\n\n\n"
		"Third one follows two of them and ends the text."_q;
}

[[nodiscard]] QString Breaks() {
	return u"https://example.com/some/very/long/path/that/must/not/break "
		"file.name.with.dots.and.more.dots.here "
		"CamelCaseWordThatIsVeryLongIndeedAndCannotBeBrokenAnywhere "
		"a-hyphenated-compound-word-that-may-break"_q;
}

[[nodiscard]] QString Ligatures() {
	// Pairs that shape into a single glyph, many of them to a line: a caret
	// inside such a glyph is placed by dividing its width, and whatever that
	// division leaves over has to stay inside it - or it pushes every letter
	// after it, the further along the line the more.
	auto result = QString();
	for (auto i = 0; i != 40; ++i) {
		result += u"fifl ffi fl affix "_q;
	}
	return result.trimmed();
}

[[nodiscard]] QString Cjk() {
	return u"这是一段中文文本用来测试换行因为中文可以在任何字符之间换行"
		"日本語のテキストもここにあります改行の位置を確認するため "
		"한국어 텍스트도 여기에 있습니다"_q;
}

// One decoration per sample: a regression in any single one of them would hide
// inside a combined sample.
[[nodiscard]] TextWithEntities Decorated(EntityType type) {
	auto result = TextWithEntities{ Latin() };
	result.entities.push_back({ type, 20, 60 });
	return result;
}

[[nodiscard]] TextWithEntities Blocks() {
	auto result = TextWithEntities{ Latin() };
	result.entities.push_back({ EntityType::Pre, 0, 40 });
	result.entities.push_back({ EntityType::Blockquote, 60, 60 });
	return result;
}

[[nodiscard]] TextWithEntities Scripts() {
	auto result = TextWithEntities{ Latin() };
	result.entities.push_back({ EntityType::Subscript, 10, 12 });
	result.entities.push_back({ EntityType::Superscript, 30, 12 });
	result.entities.push_back({ EntityType::Marked, 60, 20 });
	return result;
}

[[nodiscard]] TextWithEntities Markup() {
	auto result = TextWithEntities{ Latin() };
	result.entities.push_back({ EntityType::Bold, 4, 5 });
	result.entities.push_back({ EntityType::Italic, 16, 3 });
	result.entities.push_back({ EntityType::Code, 26, 5 });
	result.entities.push_back({ EntityType::Url, 42, 8 });
	result.entities.push_back({ EntityType::Underline, 60, 6 });
	result.entities.push_back({ EntityType::StrikeOut, 80, 7 });
	return result;
}

} // namespace

// Inline objects are the one thing QTextLayout cannot express and
// QTextDocument can, so this is what decides between them for emoji-heavy
// text. Q_OBJECT is required: registerHandler qobject_casts to the interface.
constexpr auto kBenchObjectType = QTextFormat::UserObject + 1;

class BenchObjectHandler final : public QObject, public QTextObjectInterface {
public:
	// Hand-written instead of Q_OBJECT/Q_INTERFACES, the way
	// Ui::CustomFieldObject does it, so that this needs no moc.
	void *qt_metacast(const char *iid) override {
		if (QLatin1String(iid)
			== qobject_interface_iid<QTextObjectInterface*>()) {
			return static_cast<QTextObjectInterface*>(this);
		}
		return QObject::qt_metacast(iid);
	}

	QSizeF intrinsicSize(
			QTextDocument *document,
			int position,
			const QTextFormat &format) override {
		return QSizeF(kEmojiSide, kEmojiSide);
	}
	void drawObject(
			QPainter *painter,
			const QRectF &rect,
			QTextDocument *document,
			int position,
			const QTextFormat &format) override {
		painter->drawImage(rect.topLeft(), SharedEmojiImage());
	}
};

namespace crl {

rpl::producer<> on_main_update_requests() {
	return rpl::never<>();
}

} // namespace crl

int main(int argc, char *argv[]) {
	// The official build ships no offscreen platform plugin, so let the
	// environment pick; nothing here creates a window anyway.
	QApplication app(argc, argv);

	auto baseIntegration = BaseIntegration(argc, argv);
	base::Integration::Set(&baseIntegration);

	auto integration = Integration();
	Ui::Integration::Set(&integration);

	new Ui::Animations::Manager();

	const auto iterations = (argc > 1) ? std::atoi(argv[1]) : 2000;
	const auto width = (argc > 2) ? std::atoi(argv[2]) : 320;

	// A path renders every sample into that directory and exits: two builds
	// can then be compared pixel by pixel, which is the only check that a
	// refactor of the engine changed nothing.
	//
	// `--history <file>` measures that exported history instead of the
	// samples, which is the number they are all there to explain.
	//
	// `--rich` lays the history out with the formatting the export carries -
	// links, bold, code - which is what makes blocks of a message; without it
	// only the text is taken, which is what every measurement before this used.
	//
	// `--only layout` and `--only paint` run one of the two phases and leave
	// the other at a single pass, so that a profile of the run is a profile of
	// that phase and nothing has to be told apart by its stack afterwards.
	//
	// `--font <family>` lays everything out with a font of the system instead
	// of the one the application carries, which is how the two engines can be
	// held against each other on the same one - the fonts an application adds
	// are its own and a configuration does not reach them.
	auto dumpTo = QString();
	auto history = QString();
	auto family = QString();
	auto rich = false;
	auto only = QString();
	for (auto i = 3; i != argc; ++i) {
		const auto argument = QLatin1String(argv[i]);
		if (argument == u"--history"_q && i + 1 != argc) {
			history = QString::fromLocal8Bit(argv[++i]);
		} else if (argument == u"--font"_q && i + 1 != argc) {
			family = QString::fromLocal8Bit(argv[++i]);
		} else if (argument == u"--rich"_q) {
			rich = true;
		} else if (argument == u"--only"_q && i + 1 != argc) {
			only = QString::fromLocal8Bit(argv[++i]);
		} else if (dumpTo.isEmpty() && !argument.startsWith('-')) {
			dumpTo = QString::fromLocal8Bit(argv[i]);
		}
	}
	if (!family.isEmpty()) {
		style::SetCustomFont(family);
	}

	style::internal::StartFonts();
	style::StartManager(style::kScaleDefault);

	// Native emoji come from a sprite the application generates on first run,
	// so without this they parse into blocks that draw nothing.
	Ui::Emoji::Init();

	const auto &st = st::defaultTextStyle;

	struct Case {
		std::string name;
		QString plain;
		TextWithEntities marked;
		bool useMarked = false;
		Ui::Text::MarkedContext context;
		bool skipBlock = false;
	};
	auto cases = std::vector<Case>();
	cases.push_back({ "latin", Latin() });
	cases.push_back({ "cyrillic", Cyrillic() });
	cases.push_back({ "arabic", Arabic() });
	cases.push_back({ "devanagari", Devanagari() });
	cases.push_back({ "bengali", Bengali() });
	cases.push_back({ "markup", QString(), Markup(), true });
	cases.push_back({ "bold", QString(), Decorated(EntityType::Bold), true });
	cases.push_back({ "italic", QString(), Decorated(EntityType::Italic), true });
	cases.push_back({
		"underline",
		QString(),
		Decorated(EntityType::Underline),
		true,
	});
	cases.push_back({
		"strikeout",
		QString(),
		Decorated(EntityType::StrikeOut),
		true,
	});
	cases.push_back({ "mono", QString(), Decorated(EntityType::Code), true });
	cases.push_back({ "link", QString(), Decorated(EntityType::Url), true });
	cases.push_back({ "blocks", QString(), Blocks(), true });
	cases.push_back({ "scripts", QString(), Scripts(), true });
	cases.push_back({ "spaces", Spaces() });
	cases.push_back({ "paragraphs", Paragraphs() });
	cases.push_back({ "breaks", Breaks() });
	cases.push_back({ "ligatures", Ligatures() });
	cases.push_back({ "cjk", Cjk() });
	{
		auto longText = QString();
		for (auto i = 0; i != 8; ++i) {
			longText += Latin();
			longText += ' ';
		}
		cases.push_back({ "latin-x8", longText });
	}
	{
		// Long enough to be split into several items, which is what makes
		// middle elision place the halves of a right-to-left line.
		auto longText = QString();
		for (auto i = 0; i != 4; ++i) {
			longText += Arabic();
			longText += ' ';
		}
		cases.push_back({ "arabic-x4", longText });
	}

	// Everything above is object-free; the frame and held-memory sections
	// below only use those, since a screenful of 1000-emoji messages is not
	// a thing. The emoji cases are here for the per-sample table.
	const auto plainCount = int(cases.size());
	for (const auto count : { 10, 100, 1000 }) {
		cases.push_back({
			("emoji-" + std::to_string(count)),
			QString(),
			CustomEmojiText(count, false),
			true,
			EmojiContext(),
		});
		cases.push_back({
			("emoji-" + std::to_string(count) + "-words"),
			QString(),
			CustomEmojiText(count, true),
			true,
			EmojiContext(),
		});
		cases.push_back({
			("native-emoji-" + std::to_string(count)),
			NativeEmojiText(count, false),
		});
		cases.push_back({
			("native-emoji-" + std::to_string(count) + "-words"),
			NativeEmojiText(count, true),
		});
		cases.push_back({
			("native-emoji-" + std::to_string(count) + "-spaced"),
			NativeEmojiText(count, false, true),
		});
	}

	// The gap a message bubble leaves for its info line - time, the edited mark,
	// the ticks - which Message::refreshInfoSkipBlock puts into the text as a
	// Skip block. It is the only thing that makes a text with a direction of its
	// own follow the interface direction: the renderer moves a trailing Skip item
	// to the visual front when the interface is right-to-left, and the block is
	// preceded by an added newline when the text runs the other way.
	cases.push_back({ "latin-skip", Latin(), {}, false, {}, true });
	cases.push_back({ "arabic-skip", Arabic(), {}, false, {}, true });

	// A real history instead of the samples: they are all there to explain a
	// number, and this is that number.
	if (!history.isEmpty()) {
		MeasureHistory(
			history,
			st,
			width,
			iterations,
			rich,
			only == u"layout"_q,
			only == u"paint"_q);
		return 0;
	}

	auto image = QImage(
		width * 2,
		2048,
		QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(1.);
	image.fill(Qt::transparent);

	std::printf("Qt %s | width %d | iterations %d\n\n",
		qVersion(),
		width,
		iterations);
	std::printf("%-16s %11s %10s %8s %10s %8s %6s %6s\n",
		"sample", "cold ns", "layout ns", "wrap ns", "paint ns",
		"chars", "height", "maxw");
	std::printf("%s\n", std::string(86, '-').c_str());

	if (!dumpTo.isEmpty()) {
		QDir().mkpath(dumpTo);
	}
	for (auto &entry : cases) {
		// Default minResizeWidth is kQFixedMax, which suppresses wrapping.
		auto text = Ui::Text::String(32);
		const auto assign = [&] {
			if (entry.useMarked) {
				text.setMarkedText(
					st,
					entry.marked,
					kMarkupTextOptions,
					entry.context);
			} else {
				text.setText(st, entry.plain);
			}
			if (entry.skipBlock) {
				text.updateSkipBlock(kSkipBlockWidth, st.font->height);
			}
		};
		assign();
		if (text.hasLinks()) {
			text.setLink(1, std::make_shared<LambdaClickHandler>([] {}));
		}

		const auto height = text.countHeight(width);
		// What opening a history costs: every text is laid out for the first
		// time there, so no shaping cache can help. Measure() warms up before
		// it times, which hides exactly that, hence the loop by hand with the
		// cache dropped outside the timed part.
		const auto cold = [&] {
			auto total = 0.;
			for (auto i = 0; i != iterations; ++i) {
				DropShapingCache();
				const auto start = std::chrono::steady_clock::now();
				assign();
				const auto elapsed = std::chrono::steady_clock::now() - start;
				total += std::chrono::duration<double, std::nano>(
					elapsed).count();
			}
			return total / iterations;
		}();
		const auto layout = Measure(iterations, assign);
		const auto wrap = Measure(iterations, [&] {
			(void)text.countHeight(width);
		});

		auto painter = QPainter(&image);
		painter.setPen(Qt::black);
		auto context = Ui::Text::PaintContext();
		context.position = QPoint(0, 0);
		context.outerWidth = width;
		context.availableWidth = width;
		context.palette = &st::defaultTextPalette;
		const auto paint = Measure(iterations, [&] {
			text.draw(painter, context);
		});
		painter.end();

		if (!dumpTo.isEmpty()) {
			// Every artifact as bytes, so that the right-to-left pass below can
			// compare against this one before it writes anything.
			const auto artifacts = [&](const QString &suffix) {
				const auto name = QString::fromStdString(entry.name) + suffix;
				const auto shotHeight = text.countHeight(width);
				auto result = std::vector<std::pair<QString, QByteArray>>();
				// A margin on every side, so that anything drawn outside the
				// width and height the text was given shows up in the shot -
				// in the application it would land on whatever is next to it.
				auto shot = QImage(
					width + 2 * kShotMargin,
					std::max(shotHeight, 1) + 2 * kShotMargin,
					QImage::Format_ARGB32_Premultiplied);
				auto shotContext = Ui::Text::PaintContext();
				shotContext.position = QPoint(kShotMargin, kShotMargin);
				shotContext.outerWidth = width + 2 * kShotMargin;
				shotContext.availableWidth = width;
				shotContext.palette = &st::defaultTextPalette;
				const auto png = [&](
						const QString &tail,
						const Ui::Text::PaintContext &context) {
					shot.fill(Qt::transparent);
					auto painter = QPainter(&shot);
					painter.setPen(Qt::black);
					text.draw(painter, context);
					painter.end();
					auto bytes = QByteArray();
					auto buffer = QBuffer(&bytes);
					buffer.open(QIODevice::WriteOnly);
					shot.save(&buffer, "PNG");
					result.push_back({ name + tail, bytes });
				};
				png(u".png"_q, shotContext);

				// A selection over the middle, which is the only way
				// findSelectTextRange() and the spoiler ranges get exercised.
				auto selectedContext = shotContext;
				selectedContext.selection = {
					uint16(text.length() / 4),
					uint16(text.length() * 3 / 4),
				};
				png(u"-selected.png"_q, selectedContext);

				// Everything selected, so every line end is inside the
				// selection: that is where the background has to reach past
				// the last advance to cover a glyph that sticks out, without
				// painting over the glyph itself.
				auto allSelectedContext = shotContext;
				allSelectedContext.selection = {
					uint16(0),
					uint16(text.length()),
				};
				png(u"-selectedall.png"_q, allSelectedContext);

				// Elision has its own geometry paths, including a middle one
				// that cuts the drawn range from both ends.
				for (const auto middle : { false, true }) {
					auto elidedContext = shotContext;
					elidedContext.elisionLines = middle ? 1 : 2;
					elidedContext.elisionMiddle = middle;
					png(
						middle ? u"-elidedmiddle.png"_q : u"-elided.png"_q,
						elidedContext);

					// A selection over an elided line: the drawn range is cut
					// there, and the selection has to follow the cut instead
					// of the characters the item started with.
					auto elidedSelected = elidedContext;
					elidedSelected.selection = selectedContext.selection;
					png(
						(middle
							? u"-elidedmiddle-selected.png"_q
							: u"-elided-selected.png"_q),
						elidedSelected);
				}

				// A width that leaves no room for a whole letter in either
				// half of a middle elision, which is where the cut lands on
				// the very edge of the item.
				{
					auto narrow = shotContext;
					narrow.elisionLines = 1;
					narrow.elisionMiddle = true;
					narrow.availableWidth = st.font->elidew + st.font->height;
					narrow.selection = selectedContext.selection;
					png(u"-elidedmiddle-narrow.png"_q, narrow);
				}

				// A highlight asks the same question a selection does, from
				// its own call site, and answers with a path - which is the
				// only place its geometry can be seen at all.
				auto highlights = QByteArray();
				for (const auto middle : { false, true }) {
					auto path = QPainterPath();
					auto info = Ui::Text::HighlightInfoRequest{
						.range = selectedContext.selection,
						.outPath = &path,
					};
					auto context = shotContext;
					context.highlight = &info;
					context.elisionLines = middle ? 1 : 0;
					context.elisionMiddle = middle;

					shot.fill(Qt::transparent);
					auto painter = QPainter(&shot);
					painter.setPen(Qt::black);
					text.draw(painter, context);
					painter.end();

					highlights += middle ? "middle" : "plain";
					for (const auto &polygon : path.toSubpathPolygons()) {
						for (const auto &point : polygon) {
							highlights += ' '
								+ QByteArray::number(qRound(point.x()))
								+ ',' + QByteArray::number(qRound(point.y()));
						}
						highlights += ';';
					}
					highlights += '\n';
				}
				result.push_back({ name + u".highlight"_q, highlights });

				// Hit testing is invisible in a PNG, so record the whole
				// x -> symbol mapping: this is what decides which characters a
				// drag actually selects.
				auto hits = QByteArray();
				// The default request only looks up links, so the symbol branch
				// would never run and the dump would be all zeroes.
				auto request = Ui::Text::StateRequest();
				request.flags = Ui::Text::StateRequest::Flag::LookupSymbol
					| Ui::Text::StateRequest::Flag::LookupLink;
				const auto lineHeight = st.font->height;
				for (auto y = lineHeight / 2; y < shotHeight; y += lineHeight) {
					for (auto x = 0; x != width; ++x) {
						const auto state = text.getState(
							QPoint(x, y),
							width,
							request);
						hits += QByteArray::number(x)
							+ ' ' + QByteArray::number(y)
							+ ' ' + QByteArray::number(state.symbol)
							+ ' ' + (state.afterSymbol ? '1' : '0')
							+ ' ' + (state.uponSymbol ? '1' : '0')
							+ '\n';
					}
				}
				result.push_back({ name + u".hits"_q, hits });

				// Where a caret may stand, straight from Qt: every one of these
				// has to be reachable in the dump above, or a position was lost.
				auto boundaries = QByteArray();
				auto finder = QTextBoundaryFinder(
					QTextBoundaryFinder::Grapheme,
					text.toString());
				for (auto at = finder.position()
					; at >= 0
					; at = finder.toNextBoundary()) {
					boundaries += QByteArray::number(at) + '\n';
				}
				result.push_back({ name + u".graphemes"_q, boundaries });
				return result;
			};
			const auto write = [&](
					const std::pair<QString, QByteArray> &artifact) {
				auto file = QFile(dumpTo + '/' + artifact.first);
				if (file.open(QIODevice::WriteOnly)) {
					file.write(artifact.second);
				}
			};
			const auto plain = artifacts(QString());
			for (const auto &artifact : plain) {
				write(artifact);
			}

			// A paragraph takes its base direction from the interface when the
			// text has no strong characters of its own - a row of emoji is the
			// only such sample here - and that reverses the visual order of the
			// items a line is built from, so the whole corpus is laid out and
			// dumped a second time. Only what the direction really changes is
			// written, or every sample would gain four files that repeat it.
			style::SetRightToLeft(true);
			assign();
			const auto mirrored = artifacts(u"-rtl"_q);
			style::SetRightToLeft(false);
			assign();
			for (auto i = 0, count = int(plain.size()); i != count; ++i) {
				if (mirrored[i].second != plain[i].second) {
					write(mirrored[i]);
				}
			}
		}

		std::printf("%-16s %11.0f %10.0f %8.0f %10.0f %8d %6d %6d\n",
			entry.name.c_str(),
			cold,
			layout,
			wrap,
			paint,
			text.length(),
			height,
			text.maxWidth());
	}

	// ------------------------------------------------------ one chat frame
	//
	// A screenful of messages repainted once, which is what a scroll costs.
	// Compare against 16.7 ms if this is supposed to hold 60 fps.

	const auto build = [&](int count) {
		auto result = std::vector<Ui::Text::String>();
		result.reserve(count);
		for (auto i = 0; i != count; ++i) {
			auto text = Ui::Text::String(32);
			const auto &entry = cases[i % plainCount];
			if (entry.useMarked) {
				text.setMarkedText(
					st,
					entry.marked,
					kMarkupTextOptions,
					entry.context);
			} else {
				text.setText(st, entry.plain);
			}
			result.push_back(std::move(text));
		}
		return result;
	};

	std::printf("\n%-12s %12s %12s %10s\n",
		"messages", "frame ms", "per msg us", "of 16.7ms");
	std::printf("%s\n", std::string(50, '-').c_str());
	for (const auto count : { 10, 20, 40 }) {
		auto screen = build(count);
		auto painter = QPainter(&image);
		painter.setPen(Qt::black);
		auto context = Ui::Text::PaintContext();
		context.outerWidth = width;
		context.availableWidth = width;
		context.palette = &st::defaultTextPalette;
		const auto frame = Measure(std::max(iterations / count, 20), [&] {
			auto y = 0;
			for (const auto &text : screen) {
				context.position = QPoint(0, y);
				text.draw(painter, context);
				y = (y + 80) % 1900;
			}
		});
		painter.end();
		std::printf("%-12d %12.3f %12.1f %9.0f%%\n",
			count,
			frame / 1e6,
			frame / 1e3 / count,
			100. * frame / 16'700'000.);
	}

	// ---------------------------------------------------------- memory cost
	//
	// What a laid-out String retains, and whether painting spikes anything.

	std::printf("\n%-22s %12s %12s\n", "memory", "heap KB", "rss KB");
	std::printf("%s\n", std::string(50, '-').c_str());

	malloc_trim(0);
	const auto heapBefore = HeapInUse();
	const auto rssBefore = RssKb();
	constexpr auto kHold = 2000;
	auto held = build(kHold);
	const auto heapHeld = HeapInUse();
	const auto rssHeld = RssKb();
	std::printf("%-22s %12.1f %12lld\n",
		"2000 strings held",
		(heapHeld - heapBefore) / 1024.,
		(long long)(rssHeld - rssBefore));
	std::printf("%-22s %12.1f %12s\n",
		"  bytes per string",
		double(heapHeld - heapBefore) / kHold,
		"-");

	const auto peakBeforePaint = PeakRssKb();
	const auto heapBeforePaint = HeapInUse();
	{
		auto painter = QPainter(&image);
		painter.setPen(Qt::black);
		auto context = Ui::Text::PaintContext();
		context.outerWidth = width;
		context.availableWidth = width;
		context.palette = &st::defaultTextPalette;
		auto y = 0;
		for (const auto &text : held) {
			context.position = QPoint(0, y);
			text.draw(painter, context);
			y = (y + 80) % 1900;
		}
	}
	const auto heapAfterPaint = HeapInUse();
	const auto peakAfterPaint = PeakRssKb();
	std::printf("%-22s %12.1f %12lld\n",
		"retained by painting",
		(heapAfterPaint - heapBeforePaint) / 1024.,
		(long long)(peakAfterPaint - peakBeforePaint));

	held.clear();
	malloc_trim(0);
	std::printf("%-22s %12.1f %12lld\n",
		"after release",
		(HeapInUse() - heapBefore) / 1024.,
		(long long)(RssKb() - rssBefore));

	// -------------------------------------------- what custom emoji retain

	std::printf("\n%-22s %12s %12s %12s\n",
		"custom emoji", "per string", "per emoji", "paint us");
	std::printf("%s\n", std::string(64, '-').c_str());
	for (const auto count : { 10, 100, 1000 }) {
		for (const auto words : { false, true }) {
			constexpr auto kStrings = 50;
			const auto marked = CustomEmojiText(count, words);
			malloc_trim(0);
			const auto before = HeapInUse();
			auto strings = std::vector<Ui::Text::String>();
			strings.reserve(kStrings);
			for (auto i = 0; i != kStrings; ++i) {
				auto text = Ui::Text::String(32);
				text.setMarkedText(
					st,
					marked,
					kMarkupTextOptions,
					EmojiContext());
				strings.push_back(std::move(text));
			}
			const auto used = HeapInUse() - before;

			auto painter = QPainter(&image);
			painter.setPen(Qt::black);
			auto context = Ui::Text::PaintContext();
			context.position = QPoint(0, 0);
			context.outerWidth = width;
			context.availableWidth = width;
			context.palette = &st::defaultTextPalette;
			const auto paint = Measure(20, [&] {
				strings.front().draw(painter, context);
			});
			painter.end();

			std::printf("%-22s %12.0f %12.1f %12.1f\n",
				(std::to_string(count)
					+ (words ? " + words" : " emoji only")).c_str(),
				double(used) / kStrings,
				double(used) / kStrings / count,
				paint / 1000.);
			strings.clear();
		}
	}

	// ------------------------------- what the public-API alternatives cost
	//
	// Same text, same count, held laid out - so the per-message figure is
	// directly comparable with "bytes per string" above.

	std::printf("\n%-22s %12s %12s\n",
		"alternative", "KB total", "bytes each");
	std::printf("%s\n", std::string(50, '-').c_str());

	const auto sample = Latin();
	auto font = st.font->f;

	{
		malloc_trim(0);
		const auto before = HeapInUse();
		auto layouts = std::vector<std::unique_ptr<QTextLayout>>();
		layouts.reserve(kHold);
		for (auto i = 0; i != kHold; ++i) {
			auto layout = std::make_unique<QTextLayout>(sample, font);
			layout->beginLayout();
			auto y = 0.;
			while (true) {
				auto line = layout->createLine();
				if (!line.isValid()) {
					break;
				}
				line.setLineWidth(width);
				line.setPosition(QPointF(0, y));
				y += line.height();
			}
			layout->endLayout();
			layouts.push_back(std::move(layout));
		}
		const auto used = HeapInUse() - before;
		std::printf("%-22s %12.1f %12.0f\n",
			"2000 QTextLayout",
			used / 1024.,
			double(used) / kHold);
	}
	// Held vs transient is the whole question for anything built on
	// QTextLayout: one can be kept for the lifetime of the string or built per
	// stretch and dropped, the way String::draw builds and drops a StackEngine
	// per line today. Measure both shapes separately.
	const auto layOutAt = [&](QTextLayout &layout, int at) {
		layout.beginLayout();
		auto y = 0.;
		while (true) {
			auto line = layout.createLine();
			if (!line.isValid()) {
				break;
			}
			line.setLineWidth(at);
			line.setPosition(QPointF(0, y));
			y += line.height();
		}
		layout.endLayout();
	};
	const auto layOut = [&](QTextLayout &layout) {
		layOutAt(layout, width);
	};
	{
		malloc_trim(0);
		const auto before = HeapInUse();
		auto one = std::make_unique<QTextLayout>(sample, font);
		layOut(*one);
		const auto alive = HeapInUse() - before;
		one = nullptr;
		malloc_trim(0);
		const auto after = HeapInUse() - before;
		std::printf("%-22s %12.1f %12.0f\n",
			"1 QTextLayout alive",
			alive / 1024.,
			double(alive));
		std::printf("%-22s %12.1f %12.0f\n",
			"  ...then destroyed",
			after / 1024.,
			double(after));
	}
	{
		malloc_trim(0);
		const auto before = HeapInUse();
		const auto peakBefore = PeakRssKb();
		for (auto i = 0; i != kHold; ++i) {
			auto layout = QTextLayout(sample, font);
			layOut(layout);
		}
		malloc_trim(0);
		std::printf("%-22s %12.1f %12s\n",
			"2000 built+dropped",
			(HeapInUse() - before) / 1024.,
			(PeakRssKb() > peakBefore) ? "peak grew" : "peak flat");
	}
	// An object block ends a stretch, so on the public path a message with N
	// custom emoji needs N+1 QTextLayouts per paint instead of one. Compare
	// that against the "paint us" column of the custom emoji table above.
	std::printf("\n%-22s %12s %12s\n",
		"stretches per paint", "us total", "us each");
	std::printf("%s\n", std::string(50, '-').c_str());
	for (const auto pieces : { 1, 10, 100, 1000 }) {
		const auto chunk = (pieces == 1) ? sample : u"word "_q;
		const auto ns = Measure(std::max(2000 / pieces, 20), [&] {
			for (auto i = 0; i != pieces; ++i) {
				auto layout = QTextLayout(chunk, font);
				layOut(layout);
			}
		});
		std::printf("%-22d %12.1f %12.2f\n",
			pieces,
			ns / 1000.,
			ns / 1000. / pieces);
	}

	// Same scenario by memory. Built-and-dropped is what a paint does; held
	// is what caching them would cost, to compare against the ~165 KB such a
	// message already occupies as a String.
	std::printf("\n%-22s %12s %12s\n",
		"stretch memory", "bytes each", "KB total");
	std::printf("%s\n", std::string(50, '-').c_str());
	{
		const auto chunk = u"word "_q;
		malloc_trim(0);
		{
			const auto before = HeapInUse();
			auto one = std::make_unique<QTextLayout>(chunk, font);
			layOut(*one);
			const auto alive = HeapInUse() - before;
			one = nullptr;
			malloc_trim(0);
			std::printf("%-22s %12.0f %12.1f\n",
				"1 short alive",
				double(alive),
				alive / 1024.);
			std::printf("%-22s %12.0f %12s\n",
				"  ...then destroyed",
				double(HeapInUse() - before),
				"-");
		}
		{
			malloc_trim(0);
			const auto before = HeapInUse();
			const auto peakBefore = PeakRssKb();
			for (auto i = 0; i != 1000; ++i) {
				auto layout = QTextLayout(chunk, font);
				layOut(layout);
			}
			malloc_trim(0);
			std::printf("%-22s %12.0f %12s\n",
				"1000 built+dropped",
				double(HeapInUse() - before),
				(PeakRssKb() > peakBefore) ? "peak grew" : "peak flat");
		}
		// clearLayout() drops only the line data, layoutData with the shaped
		// glyphs survives - so re-wrapping a held layout should not re-shape.
		// Compare against the "wrap ns" column: 335 ns from cached Words.
		{
			auto layout = QTextLayout(sample, font);
			layOutAt(layout, width);
			auto toggle = 0;
			const auto rewrap = Measure(iterations, [&] {
				layout.clearLayout();
				layOutAt(layout, width + (toggle ^= 1));
			});
			std::printf("%-22s %12.1f %12s\n",
				"held: re-wrap ns",
				rewrap,
				"-");

			// And the same for a message chopped into short stretches, which
			// is what an emoji-heavy message would need.
			auto pieces = std::vector<std::unique_ptr<QTextLayout>>();
			pieces.reserve(1000);
			for (auto i = 0; i != 1000; ++i) {
				auto piece = std::make_unique<QTextLayout>(u"word "_q, font);
				layOutAt(*piece, width);
				pieces.push_back(std::move(piece));
			}
			auto toggle2 = 0;
			const auto rewrapPieces = Measure(20, [&] {
				const auto at = width + (toggle2 ^= 1);
				for (const auto &piece : pieces) {
					piece->clearLayout();
					layOutAt(*piece, at);
				}
			});
			std::printf("%-22s %12.1f %12s\n",
				"held: 1000 re-wrap us",
				rewrapPieces / 1000.,
				"-");
		}
		{
			malloc_trim(0);
			const auto before = HeapInUse();
			auto layouts = std::vector<std::unique_ptr<QTextLayout>>();
			layouts.reserve(1000);
			for (auto i = 0; i != 1000; ++i) {
				auto layout = std::make_unique<QTextLayout>(chunk, font);
				layOut(*layout);
				layouts.push_back(std::move(layout));
			}
			const auto used = HeapInUse() - before;
			std::printf("%-22s %12.0f %12.1f\n",
				"1000 held (cached)",
				double(used) / 1000,
				used / 1024.);
		}
	}

	{
		malloc_trim(0);
		const auto before = HeapInUse();
		auto documents = std::vector<std::unique_ptr<QTextDocument>>();
		documents.reserve(kHold);
		for (auto i = 0; i != kHold; ++i) {
			auto document = std::make_unique<QTextDocument>();
			document->setDefaultFont(font);
			document->setPlainText(sample);
			document->setTextWidth(width);
			(void)document->size();
			documents.push_back(std::move(document));
		}
		const auto used = HeapInUse() - before;
		std::printf("%-22s %12.1f %12.0f\n",
			"2000 QTextDocument",
			used / 1024.,
			double(used) / kHold);
	}

	// Same held-vs-transient split as for QTextLayout above, so the two are
	// judged by the same standard.
	const auto fillDocument = [&](QTextDocument &document) {
		document.setDefaultFont(font);
		document.setPlainText(sample);
		document.setTextWidth(width);
		(void)document.size();
	};
	{
		malloc_trim(0);
		const auto before = HeapInUse();
		auto one = std::make_unique<QTextDocument>();
		fillDocument(*one);
		const auto alive = HeapInUse() - before;
		one = nullptr;
		malloc_trim(0);
		std::printf("%-22s %12.1f %12.0f\n",
			"1 QTextDocument alive",
			alive / 1024.,
			double(alive));
		std::printf("%-22s %12.1f %12.0f\n",
			"  ...then destroyed",
			(HeapInUse() - before) / 1024.,
			double(HeapInUse() - before));
	}
	{
		malloc_trim(0);
		const auto before = HeapInUse();
		const auto peakBefore = PeakRssKb();
		for (auto i = 0; i != kHold; ++i) {
			auto document = QTextDocument();
			fillDocument(document);
		}
		malloc_trim(0);
		std::printf("%-22s %12.1f %12s\n",
			"2000 built+dropped",
			(HeapInUse() - before) / 1024.,
			(PeakRssKb() > peakBefore) ? "peak grew" : "peak flat");
		const auto ns = Measure(50, [&] {
			auto document = QTextDocument();
			fillDocument(document);
		});
		std::printf("%-22s %12.1f %12s\n",
			"  one build+drop us",
			ns / 1000.,
			"-");
	}

	// A held QTextDocument is how the class is meant to be used: keep one per
	// message, mutate it, draw it. That trades memory for paint speed, which
	// is the opposite trade from building and dropping it.
	{
		auto document = QTextDocument();
		fillDocument(document);
		auto painter = QPainter(&image);
		painter.setPen(Qt::black);
		const auto paint = Measure(iterations, [&] {
			document.drawContents(&painter);
		});
		painter.end();
		std::printf("%-22s %12.1f %12s\n",
			"held: paint us",
			paint / 1000.,
			"-");

		// Resize is the third axis: String re-wraps from cached Words, a
		// QTextDocument has to lay out again. Compare with the "wrap ns"
		// column of the first table.
		auto toggle = 0;
		const auto resize = Measure(iterations, [&] {
			document.setTextWidth(width + (toggle ^= 1));
			(void)document.size();
		});
		std::printf("%-22s %12.1f %12s\n",
			"held: resize us",
			resize / 1000.,
			"-");
	}

	// --------------------------- the object-heavy case, which is the point
	//
	// QTextLayout has to be split at every object, QTextDocument does not.
	// Compare "us build+drop" here against 4.3 us x N for QTextLayout and
	// against the "paint us" column of the custom emoji table.

	auto handler = BenchObjectHandler();
	const auto fillObjects = [&](QTextDocument &document, int count) {
		document.documentLayout()->registerHandler(kBenchObjectType, &handler);
		document.setDefaultFont(font);
		auto cursor = QTextCursor(&document);
		auto objectFormat = QTextCharFormat();
		objectFormat.setObjectType(kBenchObjectType);
		// Without an edit block every insert triggers a relayout, which is a
		// usage mistake rather than what QTextDocument inherently costs.
		cursor.beginEditBlock();
		for (auto i = 0; i != count; ++i) {
			cursor.insertText(u"word "_q);
			cursor.insertText(
				QString(QChar::ObjectReplacementCharacter),
				objectFormat);
		}
		cursor.endEditBlock();
		document.setTextWidth(width);
		(void)document.size();
	};

	std::printf("\n%-22s %12s %12s %12s\n",
		"QTextDocument objects", "alive B", "each B", "build+drop us");
	std::printf("%s\n", std::string(64, '-').c_str());
	for (const auto count : { 10, 100, 1000 }) {
		malloc_trim(0);
		const auto before = HeapInUse();
		auto one = std::make_unique<QTextDocument>();
		fillObjects(*one, count);
		const auto alive = HeapInUse() - before;
		one = nullptr;
		malloc_trim(0);
		const auto leaked = HeapInUse() - before;

		const auto ns = Measure(std::max(200 / count, 5), [&] {
			auto document = QTextDocument();
			fillObjects(document, count);
		});
		std::printf("%-22d %12.0f %12.1f %12.1f\n",
			count,
			double(alive),
			double(alive) / count,
			ns / 1000.);
		if (leaked > 4096) {
			std::printf("%-22s %12.0f\n", "  ...retained", double(leaked));
		}
	}

	return 0;
}

