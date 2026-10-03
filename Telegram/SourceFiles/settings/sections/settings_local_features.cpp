/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/sections/settings_local_features.h"

#include "core/local_features.h"
#include "core/local_profile_export.h"
#include "core/file_utilities.h"
#include "base/call_delayed.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_icon.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "settings/settings_common_session.h"
#include "settings/sections/settings_main.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/box_content_divider.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/multi_select.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_settings_local_features.h"
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>
#include <QtGui/QPainterPath>

namespace Settings {
namespace {

using Feature = Core::LocalFeature;
using namespace Builder;

constexpr auto kCollapsed = 5;
constexpr auto kPageSize = 60;
constexpr auto kRebuildDelay = crl::time(500);
constexpr auto kSearchDelay = crl::time(150);
constexpr auto kCoverDelay = crl::time(300);
constexpr auto kChartSampleCap = TimeId(12 * 3600);

[[nodiscard]] PeerId ParsePeer(const QString &value) {
	return PeerId(PeerIdHelper(value.toULongLong()));
}

[[nodiscard]] rpl::producer<bool> FeatureValue(
		not_null<Core::LocalFeatures*> model,
		Fn<bool()> value) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		model->changes()
	) | rpl::map(value) | rpl::distinct_until_changed();
}

[[nodiscard]] rpl::producer<QString> CountValue(
		not_null<Core::LocalFeatures*> model,
		Fn<int()> count) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		model->changes()
	) | rpl::map([=] {
		const auto value = count();
		return value ? QString::number(value) : QString();
	});
}

[[nodiscard]] QString ShortDuration(TimeId seconds) {
	if (seconds <= 0) {
		return u"0"_q;
	} else if (seconds < 60) {
		return tr::lng_seconds(tr::now, lt_count, seconds);
	} else if (seconds < 3600) {
		return tr::lng_minutes(tr::now, lt_count, seconds / 60);
	}
	const auto hours = tr::lng_hours(tr::now, lt_count, seconds / 3600);
	const auto minutes = (seconds % 3600) / 60;
	return minutes
		? (hours + ' ' + tr::lng_minutes(tr::now, lt_count, minutes))
		: hours;
}

[[nodiscard]] QString TimeText(TimeId time) {
	return QLocale().toString(
		QDateTime::fromSecsSinceEpoch(time).time(),
		QLocale::ShortFormat);
}

[[nodiscard]] QString WhenText(TimeId time) {
	return langDateTime(QDateTime::fromSecsSinceEpoch(time));
}

[[nodiscard]] QString ShortWhen(TimeId time) {
	const auto date = QDateTime::fromSecsSinceEpoch(time).date();
	const auto today = QDate::currentDate();
	if (date == today) {
		return TimeText(time);
	} else if (date.year() == today.year()) {
		return langDayOfMonth(date);
	}
	return QLocale().toString(date, QLocale::ShortFormat);
}

[[nodiscard]] QString MediaText(const QString &media) {
	if (media.isEmpty()) {
		return QString();
	}
	return media.startsWith(u"photo:"_q)
		? tr::lng_in_dlg_photo(tr::now)
		: tr::lng_in_dlg_file(tr::now);
}

void PaintCard(QPainter &p, const QRect &rect) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(st::windowBgOver);
	p.drawRoundedRect(rect, st::localCardRadius, st::localCardRadius);
}

not_null<Ui::VerticalLayout*> AddCard(
		not_null<Ui::VerticalLayout*> container,
		const style::margins &margin) {
	const auto card = container->add(
		object_ptr<Ui::VerticalLayout>(container),
		margin);
	card->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(card);
		PaintCard(p, card->rect());
	}, card->lifetime());
	return card;
}

void AddNote(not_null<Ui::VerticalLayout*> container, const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::localNoteLabel),
		st::localNotePadding);
}

void AddCallout(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	const auto card = AddCard(container, st::localBlockPadding);
	card->add(
		object_ptr<Ui::FlatLabel>(card, rpl::single(text), st::localCallout),
		st::localCalloutPadding);
}

void AddGroupTitle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(text),
			st::localGroupTitle),
		st::localGroupTitlePadding);
}

// A single logged value with its date in a muted line underneath.
void AddEntry(
		not_null<Ui::VerticalLayout*> container,
		const QString &value,
		const QString &details) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(value),
			st::localEntryValue),
		st::localEntryPadding)->setSelectable(true);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(details),
			st::localEntryDate),
		st::localEntryDatePadding);
}

void PaintOnlineDot(QPainter &p, QRect userpic) {
	const auto size = st::localOnlineDot;
	const auto stroke = st::localOnlineDotStroke;
	const auto rect = QRect(
		userpic.x() + userpic.width() - size + stroke,
		userpic.y() + userpic.height() - size + stroke,
		size,
		size);
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(QPen(st::boxBg->c, stroke));
	p.setBrush(st::dialogsOnlineBadgeFg);
	p.drawEllipse(rect);
}

struct RowDescriptor {
	PeerData *peer = nullptr;
	QString title;
	QString subtitle;
	QString right;
	QString badge;
	bool online = false;
	bool subtitleActive = false;
	bool rightActive = false;
	bool badgeAttention = false;
};

class ListRow final : public Ui::RippleButton {
public:
	ListRow(QWidget *parent, RowDescriptor descriptor)
	: RippleButton(parent, st::defaultRippleAnimation)
	, _data(std::move(descriptor)) {
		if (_data.peer) {
			_data.peer->session().downloaderTaskFinished(
			) | rpl::on_next([=] {
				update();
			}, lifetime());
		}
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::localRowHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto over = isOver() || isDown();
		p.fillRect(e->rect(), over ? st::windowBgOver : st::boxBg);
		paintRipple(p, 0, 0);

		const auto size = st::localRowUserpic;
		const auto userpic = QRect(
			st::localRowPadding.left(),
			(height() - size) / 2,
			size,
			size);
		if (_data.peer) {
			_data.peer->paintUserpic(
				p,
				_view,
				userpic.x(),
				userpic.y(),
				size);
			if (_data.online) {
				PaintOnlineDot(p, userpic);
			}
		}
		const auto left = st::localRowTextLeft;
		const auto right = st::localRowPadding.right();
		const auto outer = width();
		auto rightWidth = 0;
		if (!_data.right.isEmpty()) {
			p.setFont(st::localRowStatusFont);
			p.setPen(_data.rightActive
				? st::windowActiveTextFg
				: st::windowSubTextFg);
			rightWidth = st::localRowStatusFont->width(_data.right);
			p.drawTextLeft(
				outer - right - rightWidth,
				st::localRowNameTop,
				outer,
				_data.right);
		}
		auto badgeWidth = 0;
		if (!_data.badge.isEmpty()) {
			const auto &font = st::localRowBadgeFont;
			const auto &padding = st::localRowBadgePadding;
			badgeWidth = font->width(_data.badge)
				+ padding.left()
				+ padding.right();
			const auto badgeHeight = font->height
				+ padding.top()
				+ padding.bottom();
			const auto rect = QRect(
				outer - right - badgeWidth,
				st::localRowStatusTop,
				badgeWidth,
				badgeHeight);
			const auto color = _data.badgeAttention
				? st::attentionButtonFg->c
				: st::windowSubTextFg->c;
			auto bg = color;
			bg.setAlphaF(0.15);
			{
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush(bg);
				p.drawRoundedRect(rect, badgeHeight / 2., badgeHeight / 2.);
			}
			p.setFont(font);
			p.setPen(color);
			p.drawTextLeft(
				rect.x() + padding.left(),
				rect.y() + padding.top(),
				outer,
				_data.badge);
			badgeWidth += st::localSkip;
		}
		const auto skip = st::localSkip;
		const auto titleWidth = outer - left - right
			- (rightWidth ? (rightWidth + skip) : 0);
		p.setFont(st::localRowNameFont);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(
			left,
			st::localRowNameTop,
			outer,
			st::localRowNameFont->elided(_data.title, titleWidth));
		const auto subtitleWidth = outer - left - right - badgeWidth;
		p.setFont(st::localRowStatusFont);
		p.setPen(_data.subtitleActive
			? st::windowActiveTextFg
			: st::windowSubTextFg);
		p.drawTextLeft(
			left,
			st::localRowStatusTop,
			outer,
			st::localRowStatusFont->elided(_data.subtitle, subtitleWidth));
	}

	QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::RectMask(size());
	}

private:
	RowDescriptor _data;
	Ui::PeerUserpicView _view;

};

// Rounded pills to switch between list filters.
class FilterChips final : public Ui::RpWidget {
public:
	FilterChips(QWidget *parent, std::vector<QString> labels, int active)
	: RpWidget(parent)
	, _labels(std::move(labels))
	, _active(active) {
		setCursor(style::cur_pointer);
	}

	[[nodiscard]] int active() const {
		return _active.current();
	}
	[[nodiscard]] rpl::producer<int> activeValue() const {
		return _active.value();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::localChipHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setFont(st::localChipFont);
		const auto rects = countRects();
		const auto radius = st::localChipHeight / 2.;
		for (auto i = 0; i != int(rects.size()); ++i) {
			const auto active = (i == _active.current());
			p.setPen(Qt::NoPen);
			p.setBrush(active ? st::windowBgActive : st::windowBgOver);
			p.drawRoundedRect(rects[i], radius, radius);
			p.setPen(active ? st::windowFgActive : st::windowSubTextFg);
			p.drawText(rects[i], Qt::AlignCenter, _labels[i]);
		}
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		const auto rects = countRects();
		for (auto i = 0; i != int(rects.size()); ++i) {
			if (rects[i].contains(e->pos())) {
				_active = i;
				update();
				return;
			}
		}
	}

private:
	[[nodiscard]] std::vector<QRect> countRects() const {
		auto result = std::vector<QRect>();
		auto left = 0;
		for (const auto &label : _labels) {
			const auto width = st::localChipFont->width(label)
				+ 2 * st::localChipPadding;
			result.emplace_back(left, 0, width, st::localChipHeight);
			left += width + st::localChipSkip;
		}
		return result;
	}

	std::vector<QString> _labels;
	rpl::variable<int> _active;

};

class StatsRow final : public Ui::RpWidget {
public:
	struct Stat {
		QString value;
		QString label;
	};

	StatsRow(QWidget *parent, std::vector<Stat> stats)
	: RpWidget(parent)
	, _stats(std::move(stats)) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::localStatHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto count = int(_stats.size());
		if (!count) {
			return;
		}
		const auto skip = st::localSkip;
		const auto single = (width() - skip * (count - 1)) / count;
		const auto &padding = st::localStatPadding;
		for (auto i = 0; i != count; ++i) {
			const auto rect = QRect(
				i * (single + skip),
				0,
				single,
				height());
			PaintCard(p, rect);
			const auto available = single - padding.left() - padding.right();
			p.setFont(st::localStatValueFont);
			p.setPen(st::windowBoldFg);
			p.drawTextLeft(
				rect.x() + padding.left(),
				padding.top(),
				width(),
				st::localStatValueFont->elided(_stats[i].value, available));
			p.setFont(st::localStatLabelFont);
			p.setPen(st::windowSubTextFg);
			p.drawTextLeft(
				rect.x() + padding.left(),
				st::localStatLabelTop,
				width(),
				st::localStatLabelFont->elided(_stats[i].label, available));
		}
	}

private:
	std::vector<Stat> _stats;

};

// Online time per hour of the day over the last 30 days.
[[nodiscard]] std::array<TimeId, 24> HourlyActivity(
		const std::vector<Core::OnlineSession> &sessions,
		TimeId now) {
	auto result = std::array<TimeId, 24>{};
	const auto since = now - 30 * 86400;
	for (const auto &session : sessions) {
		auto from = std::max(session.from, since);
		const auto till = std::min(
			session.till ? session.till : now,
			from + kChartSampleCap);
		while (from < till) {
			const auto local = QDateTime::fromSecsSinceEpoch(from);
			const auto hour = local.time().hour();
			const auto hourEnd = TimeId(QDateTime(
				local.date(),
				QTime(hour, 0)).toSecsSinceEpoch()) + 3600;
			const auto end = std::min(till, std::max(hourEnd, from + 1));
			result[hour] += end - from;
			from = end;
		}
	}
	return result;
}

class ActivityChart final : public Ui::RpWidget {
public:
	ActivityChart(QWidget *parent, std::array<TimeId, 24> values)
	: RpWidget(parent)
	, _values(values) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::localChartHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		PaintCard(p, rect());
		const auto inner = rect() - st::localChartPadding;
		p.setFont(st::localChartTitleFont);
		p.setPen(st::windowSubTextFg);
		p.drawTextLeft(
			inner.x(),
			inner.y(),
			width(),
			tr::lng_local_activity_hours(tr::now));

		const auto max = std::max(*ranges::max_element(_values), TimeId(1));
		const auto skip = st::localChartBarSkip;
		const auto bar = std::max((inner.width() - skip * 23) / 24, skip);
		const auto used = bar * 24 + skip * 23;
		const auto left = inner.x() + (inner.width() - used) / 2;
		const auto top = inner.y() + st::localChartTitleHeight;
		const auto bottom = inner.y()
			+ inner.height()
			- st::localChartLabelHeight;
		const auto chartHeight = bottom - top;
		const auto currentHour = QTime::currentTime().hour();
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		for (auto hour = 0; hour != 24; ++hour) {
			const auto value = _values[hour];
			const auto ratio = value / double(max);
			const auto barHeight = std::max(
				int(base::SafeRound(chartHeight * ratio)),
				st::localChartBarMin);
			const auto x = left + hour * (bar + skip);
			auto color = (value
				? st::windowActiveTextFg
				: st::windowSubTextFg)->c;
			color.setAlphaF(value
				? ((hour == currentHour) ? 1. : (0.4 + 0.6 * ratio))
				: 0.25);
			p.setBrush(color);
			const auto radius = std::min(bar / 2., barHeight / 2.);
			p.drawRoundedRect(
				QRect(x, bottom - barHeight, bar, barHeight),
				radius,
				radius);
		}
		p.setFont(st::localChartLabelFont);
		p.setPen(st::windowSubTextFg);
		for (const auto hour : { 0, 6, 12, 18 }) {
			const auto x = left + hour * (bar + skip);
			p.drawTextLeft(
				x,
				bottom + (st::localChartLabelHeight
					- st::localChartLabelFont->height),
				width(),
				QString::number(hour) + u":00"_q);
		}
	}

private:
	std::array<TimeId, 24> _values;

};

// Thumbnails of the profile photos saved for this person.
class AvatarsStrip final : public Ui::RpWidget {
public:
	AvatarsStrip(QWidget *parent, std::vector<QImage> images)
	: RpWidget(parent)
	, _images(std::move(images)) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto columns = perRow(newWidth);
		const auto rows = (int(_images.size()) + columns - 1) / columns;
		const auto size = st::localAvatarSize;
		return rows ? (rows * size + (rows - 1) * st::localAvatarSkip) : 0;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto columns = perRow(width());
		const auto size = st::localAvatarSize;
		const auto skip = st::localAvatarSkip;
		for (auto i = 0; i != int(_images.size()); ++i) {
			const auto rect = QRect(
				(i % columns) * (size + skip),
				(i / columns) * (size + skip),
				size,
				size);
			auto path = QPainterPath();
			path.addEllipse(rect);
			p.save();
			p.setClipPath(path);
			p.drawImage(rect, _images[i]);
			p.restore();
		}
	}

private:
	[[nodiscard]] int perRow(int width) const {
		const auto size = st::localAvatarSize;
		const auto skip = st::localAvatarSkip;
		return std::max((width + skip) / (size + skip), 1);
	}

	std::vector<QImage> _images;

};

class PhotoPreview final : public Ui::RpWidget {
public:
	PhotoPreview(QWidget *parent, QImage image)
	: RpWidget(parent)
	, _image(std::move(image)) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return std::min(
			st::localBubblePhoto,
			_image.height() * newWidth / std::max(_image.width(), 1));
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto size = _image.size().scaled(this->size(), Qt::KeepAspectRatio);
		const auto rect = QRect(QPoint(), size);
		auto path = QPainterPath();
		path.addRoundedRect(rect, st::localCardRadius, st::localCardRadius);
		p.setClipPath(path);
		p.drawImage(rect, _image);
	}

private:
	QImage _image;

};

class SessionRow final : public Ui::RpWidget {
public:
	SessionRow(QWidget *parent, QString text, QString right, bool active)
	: RpWidget(parent)
	, _text(std::move(text))
	, _right(std::move(right))
	, _active(active) {
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::localSessionHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto &padding = st::localEntryPadding;
		const auto top = (height() - st::boxTextFont->height) / 2;
		p.setFont(st::boxTextFont);
		p.setPen(_active ? st::windowActiveTextFg : st::windowFg);
		p.drawTextLeft(padding.left(), top, width(), _text);
		p.setFont(st::localRowStatusFont);
		p.setPen(st::windowSubTextFg);
		p.drawTextRight(padding.right(), top, width(), _right);
	}

private:
	QString _text;
	QString _right;
	bool _active = false;

};

class InsightsHeader final : public Ui::AbstractButton {
public:
	InsightsHeader(
		QWidget *parent,
		not_null<PeerData*> peer,
		QString name,
		std::vector<std::pair<QString, bool>> lines,
		bool online)
	: AbstractButton(parent)
	, _peer(peer)
	, _name(std::move(name))
	, _lines(std::move(lines))
	, _online(online) {
		_peer->session().downloaderTaskFinished(
		) | rpl::on_next([=] {
			update();
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto text = st::localHeaderLineTop
			+ int(_lines.size()) * st::localHeaderLineSkip
			+ st::localHeaderNameTop;
		return std::max(
			text,
			st::localHeaderUserpic + 2 * st::localHeaderNameTop);
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto size = st::localHeaderUserpic;
		const auto userpic = QRect(
			st::localHeaderUserpicLeft,
			(height() - size) / 2,
			size,
			size);
		_peer->paintUserpic(p, _view, userpic.x(), userpic.y(), size);
		if (_online) {
			PaintOnlineDot(p, userpic);
		}
		const auto left = st::localHeaderTextLeft;
		const auto available = width() - left - st::localHeaderUserpicLeft;
		const auto lines = int(_lines.size());
		const auto textHeight = st::localHeaderLineTop
			- st::localHeaderNameTop
			+ lines * st::localHeaderLineSkip;
		const auto shift = std::max((height() - textHeight) / 2, 0)
			- st::localHeaderNameTop;
		p.setFont(st::localHeaderNameFont);
		p.setPen(st::windowBoldFg);
		p.drawTextLeft(
			left,
			st::localHeaderNameTop + shift,
			width(),
			st::localHeaderNameFont->elided(_name, available));
		p.setFont(st::localHeaderFont);
		for (auto i = 0; i != lines; ++i) {
			const auto &[text, active] = _lines[i];
			p.setPen(active ? st::windowActiveTextFg : st::windowSubTextFg);
			p.drawTextLeft(
				left,
				st::localHeaderLineTop + shift + i * st::localHeaderLineSkip,
				width(),
				st::localHeaderFont->elided(text, available));
		}
	}

private:
	const not_null<PeerData*> _peer;
	const QString _name;
	const std::vector<std::pair<QString, bool>> _lines;
	const bool _online = false;
	Ui::PeerUserpicView _view;

};

void SaveAttachment(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		const QString &key,
		const QString &name) {
	const auto bytes = controller->session().localFeatures().mediaBytes(key);
	if (bytes.isEmpty()) {
		controller->showToast(tr::lng_local_media_missing(tr::now));
		return;
	}
	FileDialog::GetWritePath(
		parent.get(),
		tr::lng_local_save_media(tr::now),
		QString(),
		name,
		crl::guard(parent, [=](const QString &path) {
			if (!path.isEmpty()) {
				auto file = QFile(path);
				if (file.open(QIODevice::WriteOnly)) {
					file.write(bytes);
				}
			}
		}));
}

void AddVersion(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QJsonObject &version) {
	const auto card = AddCard(container, st::localBubbleMargin);
	const auto &padding = st::localBubblePadding;
	card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			rpl::single(title
				+ u" · "_q
				+ WhenText(TimeId(version[u"time"].toInteger()))),
			st::localBubbleMeta),
		QMargins(padding.left(), padding.top(), padding.right(), 0));
	const auto text = version[u"text"].toString();
	const auto key = version[u"file"].toString();
	const auto media = version[u"media"].toString();
	if (!key.isEmpty() && media.startsWith(u"photo:"_q)) {
		const auto image = QImage::fromData(
			controller->session().localFeatures().mediaBytes(key));
		if (!image.isNull()) {
			card->add(
				object_ptr<PhotoPreview>(card, image),
				QMargins(padding.left(), st::localSkip, padding.right(), 0));
		}
	}
	const auto shown = !text.isEmpty() ? text : MediaText(media);
	if (!shown.isEmpty()) {
		card->add(
			object_ptr<Ui::FlatLabel>(
				card,
				rpl::single(shown),
				st::localBubbleText),
			QMargins(
				padding.left(),
				st::localSkip / 2,
				padding.right(),
				0))->setSelectable(true);
	}
	if (!key.isEmpty()) {
		const auto name = version[u"filename"].toString();
		const auto link = card->add(
			object_ptr<Ui::LinkButton>(
				card,
				tr::lng_local_save_media(tr::now) + u" · "_q + name),
			QMargins(padding.left(), st::localSkip, padding.right(), 0),
			style::al_left);
		link->setClickedCallback([=] {
			SaveAttachment(link, controller, key, name);
		});
	}
	card->add(
		object_ptr<Ui::RpWidget>(card),
		QMargins(0, padding.bottom(), 0, 0));
}

void ShowMessageRecord(
		not_null<Window::SessionController*> controller,
		QJsonObject record) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_local_message_history());
		box->setWidth(st::localBoxWidth);
		const auto content = box->verticalLayout();
		if (record.isEmpty()) {
			AddNote(content, tr::lng_local_empty(tr::now));
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			return;
		}
		const auto session = &controller->session();
		const auto peerId = ParsePeer(record[u"peer"].toString());
		const auto peer = peerId ? session->data().peer(peerId).get() : nullptr;
		const auto header = content->add(object_ptr<ListRow>(content, RowDescriptor{
			.peer = peer,
			.title = record[u"chat"].toString(),
			.subtitle = WhenText(TimeId(record[u"date"].toInteger())),
		}));
		header->setAttribute(Qt::WA_TransparentForMouseEvents);
		if (record[u"deleted"].toBool()) {
			AddCallout(content, record[u"expired"].toBool()
				? tr::lng_local_badge_expired(tr::now)
				: tr::lng_local_badge_deleted(tr::now));
		}
		const auto versions = record[u"versions"].toArray();
		for (auto i = versions.size(); i > 0; --i) {
			const auto title = (i == 1)
				? tr::lng_local_version_original(tr::now)
				: (i == versions.size())
				? tr::lng_local_version_current(tr::now)
				: tr::lng_local_version_edit(
					tr::now,
					lt_number,
					QString::number(i - 1));
			AddVersion(content, controller, title, versions[i - 1].toObject());
		}
		// Older archives stored a single attachment at record level.
		if (record.contains(u"file"_q)) {
			auto attachment = record;
			attachment[u"text"] = QString();
			AddVersion(
				content,
				controller,
				tr::lng_local_save_media(tr::now),
				attachment);
		}
		Ui::AddSkip(content);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

[[nodiscard]] QString FieldTitle(const QString &field) {
	if (field == u"name") {
		return tr::lng_local_field_name(tr::now);
	} else if (field == u"username") {
		return tr::lng_local_field_username(tr::now);
	} else if (field == u"bio") {
		return tr::lng_local_field_bio(tr::now);
	} else if (field == u"photo") {
		return tr::lng_local_field_photo(tr::now);
	} else if (field == u"phone") {
		return tr::lng_local_field_phone(tr::now);
	} else if (field == u"premium") {
		return tr::lng_local_field_premium(tr::now);
	} else if (field == u"emojiStatus") {
		return tr::lng_local_field_emoji_status(tr::now);
	} else if (field == u"birthday") {
		return tr::lng_local_field_birthday(tr::now);
	} else if (field == u"lastSeenVisible") {
		return tr::lng_local_field_last_seen(tr::now);
	}
	return field;
}

[[nodiscard]] QString EventText(const QString &field, const QJsonObject &event) {
	const auto value = [&](const QJsonValue &v) -> QString {
		if (field == u"premium") {
			return v.toBool()
				? tr::lng_local_value_yes(tr::now)
				: tr::lng_local_value_no(tr::now);
		} else if (field == u"lastSeenVisible") {
			return v.toBool()
				? tr::lng_local_value_visible(tr::now)
				: tr::lng_local_value_hidden(tr::now);
		}
		const auto text = v.toString();
		if (text.isEmpty()) {
			return tr::lng_local_value_empty(tr::now);
		} else if (field == u"username") {
			return '@' + text;
		} else if (field == u"phone") {
			return Ui::FormatPhone(text);
		}
		return text;
	};
	if (field == u"photo") {
		const auto before = event[u"before"].toString();
		const auto after = event[u"after"].toString();
		return (after == u"0")
			? tr::lng_local_photo_removed(tr::now)
			: (before == u"0" || before.isEmpty())
			? tr::lng_local_photo_added(tr::now)
			: tr::lng_local_photo_changed(tr::now);
	} else if (field == u"emojiStatus") {
		const auto after = event[u"after"].toString();
		const auto before = event[u"before"].toString();
		return (after == u"0")
			? tr::lng_local_status_removed(tr::now)
			: (before == u"0")
			? tr::lng_local_status_set(tr::now)
			: tr::lng_local_status_changed(tr::now);
	} else if (field == u"bio") {
		return value(event[u"after"]) + u"\n"_q
			+ tr::lng_local_was(tr::now, lt_value, value(event[u"before"]));
	}
	return value(event[u"before"]) + u" → "_q + value(event[u"after"]);
}

void ShowOnlineSessions(
		not_null<Window::SessionController*> controller,
		PeerId id) {
	const auto session = &controller->session();
	const auto sessions = session->localFeatures().onlineSessions(id);
	const auto peer = session->data().peer(id);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_local_sessions_title());
		box->setWidth(st::localBoxWidth);
		const auto content = box->verticalLayout();
		const auto header = content->add(object_ptr<ListRow>(content, RowDescriptor{
			.peer = peer.get(),
			.title = peer->name(),
			.subtitle = tr::lng_local_profile_sessions(
				tr::now,
				lt_count,
				float64(sessions.size())),
		}));
		header->setAttribute(Qt::WA_TransparentForMouseEvents);
		const auto now = base::unixtime::now();
		auto day = QDate();
		for (auto i = sessions.rbegin(); i != sessions.rend(); ++i) {
			const auto date = QDateTime::fromSecsSinceEpoch(i->from).date();
			if (date != day) {
				day = date;
				AddGroupTitle(content, langDayOfMonthFull(date));
			}
			const auto till = i->till ? i->till : now;
			content->add(object_ptr<SessionRow>(
				content,
				TimeText(i->from) + u" – "_q
					+ (i->till
						? TimeText(i->till)
						: tr::lng_local_online_now(tr::now)),
				ShortDuration(till - i->from),
				!i->till));
		}
		if (sessions.empty()) {
			AddNote(content, tr::lng_local_empty(tr::now));
		}
		Ui::AddSkip(content);
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ExportProfileHistory(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		PeerId id) {
	const auto userId = QString::number(peerToUser(id).bare);
	FileDialog::GetWritePath(
		parent.get(),
		tr::lng_local_profile_export(tr::now),
		u"JSON (*.json)"_q,
		u"profile-history-%1.json"_q.arg(userId),
		crl::guard(controller, [=](const QString &path) {
			if (path.isEmpty()) {
				return;
			}
			const auto model = &controller->session().localFeatures();
			const auto record = model->profileRecord(id);
			if (record.isEmpty()) {
				controller->showToast(tr::lng_local_empty(tr::now));
				return;
			}
			const auto result = Core::LocalProfileExport::Create(
				record,
				userId,
				qint64(base::unixtime::now()),
				[=](const QString &key) { return model->mediaBytes(key); });
			const auto bytes = QJsonDocument(result).toJson(
				QJsonDocument::Indented);
			auto file = QSaveFile(path);
			if (!file.open(QIODevice::WriteOnly)
				|| file.write(bytes) != bytes.size()
				|| !file.commit()) {
				controller->showToast(tr::lng_local_export_failed(tr::now));
			} else {
				controller->showToast(tr::lng_local_export_done(tr::now));
			}
		}));
}

void ToggleHidden(
		not_null<Window::SessionController*> controller,
		PeerId id) {
	const auto session = &controller->session();
	const auto model = &session->localFeatures();
	model->toggleShadowBan(id);
	const auto name = session->data().peer(id)->name();
	controller->showToast(model->shadowBanned(id)
		? tr::lng_local_hidden_toast(tr::now, lt_user, name)
		: tr::lng_local_unhidden_toast(tr::now, lt_user, name));
}

void FillInsights(
		not_null<Ui::VerticalLayout*> content,
		not_null<Window::SessionController*> controller,
		PeerId id,
		std::shared_ptr<base::flat_set<QString>> expanded,
		Fn<void()> rebuild) {
	const auto session = &controller->session();
	const auto model = &session->localFeatures();
	const auto record = model->profileRecord(id);
	const auto peer = session->data().peer(id);
	const auto user = peer->asUser();
	const auto now = base::unixtime::now();
	const auto sessions = model->onlineSessions(id);
	const auto tracking = model->enabled(Feature::OnlineHistory);
	const auto onlineNow = !sessions.empty() && !sessions.back().till;

	auto lines = std::vector<std::pair<QString, bool>>();
	const auto username = user ? user->username() : QString();
	const auto idText = u"ID "_q + QString::number(peerToUser(id).bare);
	lines.emplace_back(
		username.isEmpty() ? idText : (u"@"_q + username + u" · "_q + idText),
		false);
	if (onlineNow) {
		lines.emplace_back(
			tr::lng_local_online_since(
				tr::now,
				lt_time,
				TimeText(sessions.back().from)),
			true);
	} else if (const auto last = model->lastObservedOnline(id)) {
		lines.emplace_back(
			tr::lng_local_last_online(tr::now, lt_time, WhenText(last)),
			false);
	} else if (user) {
		lines.emplace_back(
			Data::OnlineText(user, now),
			Data::OnlineTextActive(user, now));
	}
	if (user) {
		const auto estimate = model->registrationEstimate(peerToUser(id));
		if (!estimate.isEmpty()) {
			lines.emplace_back(
				tr::lng_local_registered(tr::now, lt_date, estimate),
				false);
		}
	}
	const auto name = peer->name().isEmpty()
		? record[u"name"].toString()
		: peer->name();
	const auto header = content->add(object_ptr<InsightsHeader>(
		content,
		peer,
		name,
		std::move(lines),
		onlineNow));
	header->setClickedCallback([=] {
		TextUtilities::SetClipboardText(TextForMimeData::Simple(
			QString::number(peerToUser(id).bare)));
		controller->showToast(tr::lng_local_id_copied(tr::now));
	});

	if (record[u"possibleBlock"].toBool()
		&& model->enabled(Feature::BlockHint)) {
		AddCallout(content, tr::lng_local_possible_block(tr::now));
	}

	if (tracking || !sessions.empty()) {
		const auto total = [&](int days) {
			auto result = TimeId(0);
			const auto since = now - days * 86400;
			for (const auto &session : sessions) {
				const auto till = session.till ? session.till : now;
				if (till > since) {
					result += till - std::max(session.from, since);
				}
			}
			return result;
		};
		content->add(
			object_ptr<StatsRow>(content, std::vector<StatsRow::Stat>{
				{ ShortDuration(total(7)), tr::lng_local_stat_week(tr::now) },
				{ ShortDuration(total(30)), tr::lng_local_stat_month(tr::now) },
				{
					QString::number(sessions.size()),
					tr::lng_local_stat_sessions(tr::now),
				},
			}),
			st::localBlockPadding);
		if (!sessions.empty()) {
			content->add(
				object_ptr<ActivityChart>(
					content,
					HourlyActivity(sessions, now)),
				st::localBlockPadding);
		}
	}

	Ui::AddSkip(content);
	if (tracking) {
		AddButtonWithIcon(
			content,
			tr::lng_local_watch_online(),
			st::settingsButton,
			{ &st::menuIconNotifications }
		)->toggleOn(
			rpl::single(model->watchingOnline(id))
		)->toggledChanges(
		) | rpl::filter([=](bool checked) {
			return (checked != model->watchingOnline(id));
		}) | rpl::on_next([=] {
			model->toggleWatchOnline(id);
		}, content->lifetime());
	}
	AddButtonWithIcon(
		content,
		tr::lng_local_hide_person(),
		st::settingsButton,
		{ &st::menuIconBlock }
	)->toggleOn(
		rpl::single(model->shadowBanned(id))
	)->toggledChanges(
	) | rpl::filter([=](bool checked) {
		return (checked != model->shadowBanned(id));
	}) | rpl::on_next([=] {
		ToggleHidden(controller, id);
	}, content->lifetime());
	if (!sessions.empty()) {
		AddButtonWithLabel(
			content,
			tr::lng_local_sessions_all(),
			rpl::single(QString::number(sessions.size())),
			st::settingsButton,
			{ &st::menuIconSchedule }
		)->setClickedCallback([=] {
			ShowOnlineSessions(controller, id);
		});
	}
	if (!record.isEmpty()) {
		const auto exportButton = AddButtonWithIcon(
			content,
			tr::lng_local_profile_export(),
			st::settingsButton,
			{ &st::menuIconExport });
		exportButton->setClickedCallback([=] {
			ExportProfileHistory(exportButton, controller, id);
		});
	}

	const auto events = record[u"events"].toArray();
	auto byField = std::vector<std::pair<QString, std::vector<QJsonObject>>>();
	for (const auto &field : {
			u"name"_q,
			u"username"_q,
			u"bio"_q,
			u"phone"_q,
			u"premium"_q,
			u"emojiStatus"_q,
			u"birthday"_q,
			u"lastSeenVisible"_q }) {
		auto list = std::vector<QJsonObject>();
		for (auto i = events.size(); i > 0; --i) {
			const auto event = events[i - 1].toObject();
			if (event[u"field"].toString() == field) {
				list.push_back(event);
			}
		}
		if (!list.empty()) {
			byField.emplace_back(field, std::move(list));
		}
	}
	auto photoEvents = 0;
	for (const auto &value : events) {
		if (value.toObject()[u"field"].toString() == u"photo"_q) {
			++photoEvents;
		}
	}
	auto avatars = std::vector<std::pair<qint64, QImage>>();
	for (const auto &value : record[u"avatars"].toObject()) {
		const auto avatar = value.toObject();
		auto image = QImage::fromData(
			model->mediaBytes(avatar[u"key"].toString()));
		if (!image.isNull()) {
			avatars.emplace_back(avatar[u"time"].toInteger(), std::move(image));
		}
	}
	ranges::sort(avatars, ranges::greater(), [](const auto &pair) {
		return pair.first;
	});
	const auto showAvatars = (avatars.size() > 1)
		|| (photoEvents > 0 && !avatars.empty());

	if (!byField.empty() || showAvatars) {
		Ui::AddSkip(content);
		Ui::AddDivider(content);
		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_local_changes());
	}
	if (showAvatars) {
		auto images = std::vector<QImage>();
		for (auto &[time, image] : avatars) {
			images.push_back(std::move(image));
		}
		AddGroupTitle(content, tr::lng_local_field_photo(tr::now));
		content->add(
			object_ptr<AvatarsStrip>(content, std::move(images)),
			st::localAvatarsPadding);
	}
	for (const auto &[field, list] : byField) {
		AddGroupTitle(content, FieldTitle(field));
		const auto full = expanded->contains(field);
		const auto count = int(list.size());
		const auto shown = full ? count : std::min(count, kCollapsed);
		for (auto i = 0; i != shown; ++i) {
			AddEntry(
				content,
				EventText(field, list[i]),
				WhenText(TimeId(list[i][u"time"].toInteger())));
		}
		if (!full && count > kCollapsed) {
			const auto more = content->add(
				object_ptr<Ui::SettingsButton>(
					content,
					tr::lng_local_show_more(
						lt_count,
						rpl::single(float64(count - kCollapsed))),
					st::settingsButtonLightNoIcon));
			more->setClickedCallback([=, field = field] {
				expanded->emplace(field);
				rebuild();
			});
		}
	}
	if (byField.empty() && !showAvatars && sessions.empty()) {
		Ui::AddSkip(content);
		Ui::AddDivider(content);
		AddNote(content, (model->enabled(Feature::ProfileHistory) || tracking)
			? tr::lng_local_profile_waiting(tr::now)
			: tr::lng_local_profile_disabled(tr::now));
	}
	Ui::AddSkip(content);
}

void ShowProfileInsights(
		not_null<Window::SessionController*> controller,
		PeerId id) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		box->setTitle(tr::lng_local_insights());
		box->setWidth(st::localBoxWidth);
		const auto content = box->verticalLayout();
		const auto expanded = std::make_shared<base::flat_set<QString>>();
		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			content->clear();
			FillInsights(content, controller, id, expanded, *rebuild);
			content->resizeToWidth(content->width());
		};
		(*rebuild)();
		const auto scheduled = std::make_shared<bool>(false);
		session->localFeatures().profileUpdates(
		) | rpl::filter(rpl::mappers::_1 == id) | rpl::on_next([=] {
			if (!*scheduled) {
				*scheduled = true;
				base::call_delayed(kRebuildDelay, box, [=] {
					*scheduled = false;
					(*rebuild)();
				});
			}
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

struct ListBoxState {
	not_null<Ui::VerticalLayout*> list;
	Fn<void()> rebuild;
	int limit = kPageSize;
	bool scheduled = false;
};

void ScheduleRebuild(
		not_null<Ui::GenericBox*> box,
		not_null<ListBoxState*> state,
		crl::time delay) {
	if (state->scheduled) {
		return;
	}
	state->scheduled = true;
	base::call_delayed(delay, box, [=] {
		state->scheduled = false;
		state->rebuild();
	});
}

void AddShowMore(not_null<Ui::GenericBox*> box, not_null<ListBoxState*> state) {
	const auto more = state->list->add(object_ptr<Ui::SettingsButton>(
		state->list,
		tr::lng_local_more(),
		st::settingsButtonLightNoIcon));
	more->setClickedCallback([=] {
		state->limit += kPageSize;
		ScheduleRebuild(box, state, 0);
	});
}

enum class ProfilesFilter {
	All,
	Changed,
	Watched,
};

void ShowProfilesList(
		not_null<Window::SessionController*> controller,
		ProfilesFilter initial) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		const auto model = &session->localFeatures();
		box->setTitle(tr::lng_local_tracked_profiles());
		box->setWidth(st::localBoxWidth);

		const auto top = box->setPinnedToTopContent(
			object_ptr<Ui::VerticalLayout>(box));
		const auto search = top->add(object_ptr<Ui::MultiSelect>(
			top,
			st::defaultMultiSelect,
			tr::lng_local_profile_search()));
		const auto chips = top->add(
			object_ptr<FilterChips>(
				top,
				std::vector<QString>{
					tr::lng_local_filter_all(tr::now),
					tr::lng_local_filter_changed(tr::now),
					tr::lng_local_filter_watched(tr::now),
				},
				int(initial)),
			st::localChipsPadding);

		const auto state = box->lifetime().make_state<ListBoxState>(
			ListBoxState{ .list = box->verticalLayout() });
		state->rebuild = [=] {
			const auto list = state->list;
			list->clear();
			const auto filter = ProfilesFilter(chips->active());
			const auto query = search->getQuery();
			auto matches = std::vector<QJsonObject>();
			for (const auto &value : model->profileRecords()) {
				auto record = value.toObject();
				const auto id = ParsePeer(record[u"peer"].toString());
				record[u"userId"] = QString::number(peerToUser(id).bare);
				if ((filter == ProfilesFilter::Watched
						&& !model->watchingOnline(id))
					|| (filter == ProfilesFilter::Changed
						&& record[u"events"].toArray().isEmpty())
					|| !Core::LocalProfileExport::MatchesQuery(record, query)) {
					continue;
				}
				matches.push_back(std::move(record));
			}
			Ui::AddSkip(list, st::localSkip);
			const auto count = int(matches.size());
			for (auto i = 0; i < std::min(count, state->limit); ++i) {
				const auto &record = matches[i];
				const auto id = ParsePeer(record[u"peer"].toString());
				const auto peer = session->data().peer(id);
				const auto sessions = model->onlineSessions(id);
				const auto online = !sessions.empty() && !sessions.back().till;
				const auto username = record[u"snapshot"].toObject()
					[u"username"].toString();
				auto parts = QStringList();
				if (!username.isEmpty()) {
					parts.push_back('@' + username);
				}
				if (const auto changes = record[u"events"].toArray().size()) {
					parts.push_back(tr::lng_local_profile_changes(
						tr::now,
						lt_count,
						float64(changes)));
				}
				if (!sessions.empty()) {
					parts.push_back(tr::lng_local_profile_sessions(
						tr::now,
						lt_count,
						float64(sessions.size())));
				}
				const auto name = record[u"name"].toString();
				const auto row = list->add(object_ptr<ListRow>(list, RowDescriptor{
					.peer = peer.get(),
					.title = name.isEmpty() ? record[u"userId"].toString() : name,
					.subtitle = parts.join(u" · "_q),
					.right = online
						? tr::lng_local_online_now(tr::now)
						: ShortWhen(TimeId(record[u"updated"].toInteger())),
					.online = online,
					.rightActive = online,
				}));
				row->setClickedCallback([=] {
					ShowProfileInsights(controller, id);
				});
			}
			if (matches.empty()) {
				AddNote(list, !query.trimmed().isEmpty()
					? tr::lng_local_nothing_found(tr::now)
					: (filter == ProfilesFilter::Watched)
					? tr::lng_local_watch_list_empty(tr::now)
					: tr::lng_local_empty(tr::now));
			} else if (count > state->limit) {
				AddShowMore(box, state);
			}
			Ui::AddSkip(list, st::localSkip);
			list->resizeToWidth(list->width());
		};
		search->setQueryChangedCallback([=](const QString &) {
			state->limit = kPageSize;
			ScheduleRebuild(box, state, kSearchDelay);
		});
		chips->activeValue() | rpl::skip(1) | rpl::on_next([=] {
			state->limit = kPageSize;
			ScheduleRebuild(box, state, 0);
		}, box->lifetime());
		rpl::merge(
			model->profileUpdates() | rpl::to_empty,
			model->changes()
		) | rpl::on_next([=] {
			ScheduleRebuild(box, state, kRebuildDelay);
		}, box->lifetime());
		box->setFocusCallback([=] { search->setInnerFocus(); });
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		state->rebuild();
	}));
}

void ShowMessagesArchive(
		not_null<Window::SessionController*> controller,
		PeerId chat) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		const auto model = &session->localFeatures();
		if (chat) {
			box->setTitle(rpl::single(session->data().peer(chat)->name()));
		} else {
			box->setTitle(tr::lng_local_archive_open());
		}
		box->setWidth(st::localBoxWidth);
		const auto search = box->setPinnedToTopContent(
			object_ptr<Ui::MultiSelect>(
				box,
				st::defaultMultiSelect,
				tr::lng_local_messages_search()));
		const auto state = box->lifetime().make_state<ListBoxState>(
			ListBoxState{ .list = box->verticalLayout() });
		state->rebuild = [=] {
			const auto list = state->list;
			list->clear();
			const auto tokens = search->getQuery().simplified().toCaseFolded()
				.split(' ', Qt::SkipEmptyParts);
			const auto matches = [&](const Core::LocalMessageEntry &entry) {
				if (tokens.isEmpty()) {
					return true;
				}
				const auto haystack = (entry.chat + ' ' + entry.preview)
					.toCaseFolded();
				return ranges::all_of(tokens, [&](const QString &token) {
					return haystack.contains(token);
				});
			};
			Ui::AddSkip(list, st::localSkip);
			auto shown = 0;
			auto more = false;
			for (const auto entry : model->messageEntries(chat)) {
				if (!matches(*entry)) {
					continue;
				} else if (shown == state->limit) {
					more = true;
					break;
				}
				++shown;
				using Media = Core::LocalMessageIndex::MediaKind;
				const auto text = entry->preview.simplified();
				const auto badge = entry->deleted
					? (entry->expired
						? tr::lng_local_badge_expired(tr::now)
						: tr::lng_local_badge_deleted(tr::now))
					: (entry->versions > 1)
					? tr::lng_local_badge_edited(
						tr::now,
						lt_count,
						float64(entry->versions - 1))
					: QString();
				const auto id = FullMsgId(
					PeerId(entry->peer),
					MsgId(entry->msg));
				const auto row = list->add(object_ptr<ListRow>(list, RowDescriptor{
					.peer = session->data().peer(id.peer).get(),
					.title = entry->chat,
					.subtitle = !text.isEmpty()
						? text
						: (entry->media == Media::Photo)
						? tr::lng_in_dlg_photo(tr::now)
						: (entry->media == Media::Document)
						? tr::lng_in_dlg_file(tr::now)
						: QString(),
					.right = ShortWhen(TimeId(entry->updated)),
					.badge = badge,
					.badgeAttention = entry->deleted,
				}));
				row->setClickedCallback([=] {
					ShowMessageRecord(controller, model->messageRecord(id));
				});
			}
			if (!shown) {
				AddNote(list, tokens.isEmpty()
					? tr::lng_local_empty(tr::now)
					: tr::lng_local_nothing_found(tr::now));
			} else if (more) {
				AddShowMore(box, state);
			}
			Ui::AddSkip(list, st::localSkip);
			list->resizeToWidth(list->width());
		};
		search->setQueryChangedCallback([=](const QString &) {
			state->limit = kPageSize;
			ScheduleRebuild(box, state, kSearchDelay);
		});
		box->setFocusCallback([=] { search->setInnerFocus(); });
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		state->rebuild();
	}));
}

void ShowHiddenPeople(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		const auto model = &session->localFeatures();
		box->setTitle(tr::lng_local_shadow_bans());
		box->setWidth(st::localBoxWidth);
		const auto state = box->lifetime().make_state<ListBoxState>(
			ListBoxState{ .list = box->verticalLayout() });
		state->rebuild = [=] {
			const auto list = state->list;
			list->clear();
			const auto hidden = model->shadowBans();
			Ui::AddSkip(list, st::localSkip);
			for (const auto &value : hidden) {
				const auto id = ParsePeer(value.toString());
				const auto peer = session->data().peer(id);
				const auto user = peer->asUser();
				const auto username = user ? user->username() : QString();
				const auto row = list->add(object_ptr<ListRow>(list, RowDescriptor{
					.peer = peer.get(),
					.title = peer->name().isEmpty() ? value.toString() : peer->name(),
					.subtitle = username.isEmpty()
						? (u"ID "_q + QString::number(peerToUser(id).bare))
						: ('@' + username),
					.right = tr::lng_local_unhide_person(tr::now),
					.rightActive = true,
				}));
				row->setClickedCallback([=] {
					ToggleHidden(controller, id);
				});
			}
			if (hidden.isEmpty()) {
				AddNote(list, tr::lng_local_shadow_about(tr::now));
			}
			Ui::AddSkip(list, st::localSkip);
			list->resizeToWidth(list->width());
		};
		model->changes() | rpl::on_next([=] {
			ScheduleRebuild(box, state, 0);
		}, box->lifetime());
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		state->rebuild();
	}));
}

void BuildCover(SectionBuilder &builder) {
	builder.add([](const WidgetContext &ctx) {
		const auto parent = ctx.container;
		const auto divider = Ui::CreateChild<Ui::BoxContentDivider>(
			parent.get());
		const auto layout = parent->add(
			object_ptr<Ui::VerticalLayout>(parent.get()));
		auto icon = CreateLottieIcon(
			layout,
			{
				.name = u"show_or_premium_lastseen"_q,
				.sizeOverride = {
					st::localCoverIconSize,
					st::localCoverIconSize,
				},
			},
			st::localCoverIconPadding);
		const auto widget = layout->add(std::move(icon.widget));
		base::call_delayed(kCoverDelay, widget, [=, animate = icon.animate] {
			animate(anim::repeat::once);
		});
		layout->add(
			object_ptr<Ui::FlatLabel>(
				layout,
				tr::lng_local_cover_about(),
				st::settingsFilterDividerLabel),
			st::localCoverLabelPadding,
			style::al_top)->setTryMakeSimilarLines(true);
		layout->geometryValue(
		) | rpl::on_next([=](const QRect &r) {
			divider->setGeometry(r);
		}, divider->lifetime());
		return SectionBuilder::WidgetToAdd{};
	});
}

class LocalFeaturesSection final : public Section<LocalFeaturesSection> {
public:
	LocalFeaturesSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override {
		return tr::lng_local_features();
	}

};

const auto kMeta = BuildHelper({
	.id = LocalFeaturesSection::Id(),
	.parentId = MainId(),
	.title = &tr::lng_local_features,
	.icon = &st::menuIconStealth,
}, [](SectionBuilder &builder) {
	const auto session = builder.session();
	const auto controller = builder.controller();
	const auto model = &session->localFeatures();

	const auto addToggle = [&](
			Feature feature,
			rpl::producer<QString> title,
			const style::icon &icon,
			QStringList keywords) {
		const auto button = builder.addButton({
			.id = u"local/"_q + QString::number(int(feature)),
			.title = std::move(title),
			.icon = { &icon },
			.toggled = FeatureValue(model, [=] {
				return model->enabled(feature);
			}),
			.keywords = std::move(keywords),
		});
		if (button) {
			button->toggledChanges(
			) | rpl::filter([=](bool checked) {
				return (checked != model->enabled(feature));
			}) | rpl::on_next([=](bool checked) {
				model->setEnabled(feature, checked);
			}, button->lifetime());
		}
	};
	const auto open = [=](Fn<void(not_null<Window::SessionController*>)> f) {
		return [=] {
			if (controller) {
				f(controller);
			}
		};
	};

	BuildCover(builder);

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_local_ghost());
	const auto ghost = builder.addButton({
		.id = u"local/ghost"_q,
		.title = tr::lng_local_ghost(),
		.icon = { &st::menuIconStealth },
		.toggled = FeatureValue(model, [=] { return model->ghostMode(); }),
		.keywords = { u"ghost"_q, u"stealth"_q, u"invisible"_q },
	});
	if (ghost) {
		ghost->toggledChanges(
		) | rpl::filter([=](bool checked) {
			return (checked != model->ghostMode());
		}) | rpl::on_next([=](bool checked) {
			model->setGhostMode(checked);
		}, ghost->lifetime());
	}
	addToggle(
		Feature::GhostMessages,
		tr::lng_local_ghost_messages(),
		st::menuIconMarkRead,
		{ u"read"_q, u"receipts"_q });
	addToggle(
		Feature::GhostStories,
		tr::lng_local_ghost_stories(),
		st::menuIconStoriesSavedSection,
		{ u"stories"_q, u"views"_q });
	addToggle(
		Feature::GhostOnline,
		tr::lng_local_ghost_online(),
		st::menuIconWhenOnline,
		{ u"online"_q, u"typing"_q, u"status"_q });
	builder.addSkip();
	builder.addDividerText(tr::lng_local_ghost_about());

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_local_archive());
	addToggle(
		Feature::EditHistory,
		tr::lng_local_edit_history(),
		st::menuIconEdit,
		{ u"edit"_q, u"versions"_q });
	addToggle(
		Feature::KeepDeleted,
		tr::lng_local_keep_deleted(),
		st::menuIconDelete,
		{ u"deleted"_q });
	addToggle(
		Feature::KeepExpired,
		tr::lng_local_keep_expired(),
		st::menuIconTimer,
		{ u"self-destructing"_q, u"ttl"_q });
	addToggle(
		Feature::CopyProtected,
		tr::lng_local_copy_protected(),
		st::menuIconCopy,
		{ u"protected"_q, u"screenshot"_q, u"copy"_q });
	builder.addButton({
		.id = u"local/archive"_q,
		.title = tr::lng_local_archive_open(),
		.icon = { &st::menuIconArchive },
		.label = CountValue(model, [=] { return model->messageCount(); }),
		.onClick = open([](not_null<Window::SessionController*> window) {
			ShowMessagesArchive(window, PeerId());
		}),
		.keywords = { u"deleted"_q, u"edited"_q, u"archive"_q },
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_local_archive_about());

	builder.addSkip();
	builder.addSubsectionTitle(tr::lng_local_profiles());
	addToggle(
		Feature::ProfileHistory,
		tr::lng_local_track_profiles(),
		st::menuIconProfile,
		{ u"profile"_q, u"name"_q, u"username"_q, u"avatar"_q });
	addToggle(
		Feature::OnlineHistory,
		tr::lng_local_track_online(),
		st::menuIconStats,
		{ u"online"_q, u"sessions"_q, u"activity"_q });
	addToggle(
		Feature::BlockHint,
		tr::lng_local_block_hint(),
		st::menuIconReport,
		{ u"blocked"_q });
	addToggle(
		Feature::RegistrationEstimate,
		tr::lng_local_registration(),
		st::menuIconSchedule,
		{ u"registration"_q, u"age"_q });
	builder.addButton({
		.id = u"local/profiles"_q,
		.title = tr::lng_local_tracked_profiles(),
		.icon = { &st::menuIconUserShow },
		.label = CountValue(model, [=] { return model->profileCount(); }),
		.onClick = open([](not_null<Window::SessionController*> window) {
			ShowProfilesList(window, ProfilesFilter::All);
		}),
		.keywords = { u"profiles"_q, u"insights"_q },
	});
	builder.addButton({
		.id = u"local/alerts"_q,
		.title = tr::lng_local_watch_list(),
		.icon = { &st::menuIconNotifications },
		.label = CountValue(model, [=] { return model->watchedCount(); }),
		.onClick = open([](not_null<Window::SessionController*> window) {
			ShowProfilesList(window, ProfilesFilter::Watched);
		}),
		.keywords = { u"alerts"_q, u"online"_q, u"notify"_q },
	});
	builder.addButton({
		.id = u"local/hidden"_q,
		.title = tr::lng_local_shadow_bans(),
		.icon = { &st::menuIconBlock },
		.label = CountValue(model, [=] {
			return int(model->shadowBans().size());
		}),
		.onClick = open(ShowHiddenPeople),
		.keywords = { u"hidden"_q, u"hide"_q },
	});
	builder.addButton({
		.id = u"local/clear"_q,
		.title = tr::lng_local_clear_profiles(),
		.st = &st::settingsAttentionButtonWithIcon,
		.icon = { &st::menuIconDeleteAttention },
		.onClick = open([=](not_null<Window::SessionController*> window) {
			window->show(Ui::MakeConfirmBox({
				.text = tr::lng_local_clear_profiles_sure(),
				.confirmed = [=](Fn<void()> close) {
					model->clearProfiles();
					close();
				},
				.confirmText = tr::lng_local_clear(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}),
	});
	builder.addSkip();
	builder.addDividerText(tr::lng_local_profiles_about());
	builder.addSkip();
});

LocalFeaturesSection::LocalFeaturesSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

} // namespace

Type LocalFeaturesId() {
	return LocalFeaturesSection::Id();
}

void ShowLocalMessageHistory(
		not_null<Window::SessionController*> controller,
		FullMsgId id) {
	ShowMessageRecord(
		controller,
		controller->session().localFeatures().messageRecord(id));
}

void ShowLocalMessagesArchive(
		not_null<Window::SessionController*> controller,
		PeerId chat) {
	ShowMessagesArchive(controller, chat);
}

void ShowLocalProfileInsights(
		not_null<Window::SessionController*> controller,
		PeerId id) {
	ShowProfileInsights(controller, id);
}

void ToggleLocalHidden(
		not_null<Window::SessionController*> controller,
		PeerId id) {
	ToggleHidden(controller, id);
}

bool HasLocalInsights(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return user
		&& !user->isSelf()
		&& !user->isBot()
		&& !user->isServiceUser()
		&& !user->isInaccessible()
		&& !user->isRepliesChat()
		&& !user->isVerifyCodes();
}

void AddLocalSenderActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item) {
	const auto sender = item->from();
	if (!HasLocalInsights(sender) || sender == item->history()->peer) {
		return;
	}
	const auto id = sender->id;
	const auto hidden = sender->session().localFeatures().shadowBanned(id);
	menu->addSeparator();
	menu->addAction(tr::lng_local_insights(tr::now), [=] {
		ShowProfileInsights(controller, id);
	}, &st::menuIconStats);
	menu->addAction(hidden
		? tr::lng_local_unhide_person(tr::now)
		: tr::lng_local_hide_person(tr::now), [=] {
		ToggleHidden(controller, id);
	}, hidden ? &st::menuIconUnblock : &st::menuIconBlock);
}

} // namespace Settings
