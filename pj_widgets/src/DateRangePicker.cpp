// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Date/time range picker bundle, wrapped in namespace PJ. Theming is driven from
// the persisted-theme tokens (pickerTokens); the month-nav arrows use the app's
// Material "keyboard_arrow_left/right" chevron SVGs (recolored to the theme ink,
// or the from/to hint colors) so they read big and match the rest of the chrome.
// See DateRangePicker.h.

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/IntScrubber.h>
#include <pj_widgets/SvgButton.h>

#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QString>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <algorithm>
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

constexpr int kRows = 6;
constexpr int kCols = 7;
constexpr int kHeaderPad = 8;

// Month-nav chevron icon size — deliberately large so the arrows read as the same
// chevrons used elsewhere in the chrome, not thin punctuation glyphs.
constexpr int kNavChevronPx = 24;

const QColor kFromHintColor(0x2e, 0xcc, 0x71);  // green
const QColor kToHintColor(0xe7, 0x4c, 0x3c);    // red

// The app persists its active theme to QSettings("StyleSheet::theme")
// (Theme.cpp) — the same key the rest of pj_dialog_host (widget_binding.cpp)
// reads. We key off it because the calendar paints from palette() roles, but
// the app pins QPalette at Fusion defaults regardless of the active QSS theme,
// so palette() resolves to Fusion light-grey on both themes. Sourcing colors
// from theme tokens chosen by this key is the fix.
bool pickerThemeIsLight() {
  return QSettings().value(u"StyleSheet::theme"_s, u"light"_s).toString().contains("light");
}

// Theme-token color set for the calendar/overlay. Values mirror the app's QSS
// tokens (light / dark) so the picker reads correctly on both themes. The
// semantic accents (weekend red, from-green, to-red, white-on-endpoint) stay
// hardcoded at their call sites — they are theme-agnostic by design.
struct PickerTokens {
  QColor surface;          // calendar body + overlay background
  QColor text;             // month/year title, day numbers
  QColor muted;            // weekday header row, grid lines
  QColor border;           // overlay frame border, grid
  QColor selection_range;  // in-range fill (used at alpha ~60)
  QColor hover_fill;       // hover cell fill
  QColor hover_border;     // hover cell border (== border)
};

PickerTokens pickerTokens() {
  PickerTokens t;
  if (pickerThemeIsLight()) {
    t.surface = QColor(0xFF, 0xFF, 0xFF);          // pure white calendar/overlay surface
    t.text = QColor(0x11, 0x11, 0x11);             // #111111  default_text
    t.muted = QColor(0x66, 0x66, 0x66);            // #666666  disabled_text
    t.border = QColor(0xc0, 0xc0, 0xc0);           // #c0c0c0  border_default
    t.selection_range = QColor(0xC2, 0xDC, 0xFF);  // #C2DCFF  item_selection_background
    t.hover_fill = QColor(0, 0, 0, 40);            // rgba(0,0,0,40) hover_overlay
  } else {
    t.surface = QColor(0x3B, 0x3B, 0x47);          // #3B3B47  dark_background
    t.text = QColor(0xF0, 0xF0, 0xF0);             // #F0F0F0  default_text
    t.muted = QColor(0x77, 0x77, 0x77);            // #777777  disabled_text
    t.border = QColor(0xB0, 0xB0, 0xBF);           // #B0B0BF  border_default
    t.selection_range = QColor(0x14, 0x8C, 0xD2);  // #148CD2  item_selection_background
    t.hover_fill = QColor(255, 255, 255, 30);      // rgba(255,255,255,30) hover_overlay
  }
  t.hover_border = t.border;
  return t;
}

// Recolor a monochrome icon SVG to the active theme's ink. Local copy of
// widget_binding.cpp::recolorSvgInk (kept local because pj_widgets/SvgUtil.h is
// not on this module's include path): swaps #3D3D3D<->#E0E0E0 so the icon
// authored for one theme renders in the other's ink, plus legacy black/white.
void recolorSvgInk(QByteArray& svg_data, bool light_theme) {
  const QByteArray ink = light_theme ? QByteArray("#3D3D3D") : QByteArray("#E0E0E0");
  const QByteArray opposite = light_theme ? QByteArray("#E0E0E0") : QByteArray("#3D3D3D");
  svg_data.replace(opposite, ink);   // palette swap (icon authored for the other theme)
  svg_data.replace("#000000", ink);  // legacy black -> ink
  svg_data.replace("#ffffff", opposite);
  const qsizetype svg_open = svg_data.indexOf("<svg");
  if (svg_open < 0) {
    return;
  }
  const qsizetype tag_end = svg_data.indexOf('>', svg_open);
  if (tag_end <= svg_open) {
    return;
  }
  // Inject a root fill only when none is present, so fill-less Material paths
  // inherit the theme ink.
  if (svg_data.mid(svg_open, tag_end - svg_open).contains("fill=\"")) {
    return;
  }
  svg_data.insert(tag_end, " fill=\"" + ink + "\"");
}

// Rasterize already-prepared SVG bytes into a px-logical QIcon at size*DPR so it
// stays crisp on HiDPI. Callers own the theming/recoloring of `svg_data`; this is
// the shared QSvgRenderer + DPR-scaling tail behind both icon paths below (and
// mirrors widget_binding.cpp's render path).
QIcon rasterizeSvgIcon(const QByteArray& svg_data, int px) {
  QSvgRenderer renderer(svg_data);
  if (!renderer.isValid()) {
    return QIcon();
  }
  const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
  QImage image(QSize(px, px) * dpr, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  QPixmap pix = QPixmap::fromImage(image);
  pix.setDevicePixelRatio(dpr);
  return QIcon(pix);
}

// Render a qrc SVG into a theme-inked QIcon.
QIcon renderThemedIcon(const QString& resource_path, int px = 16) {
  QFile file(resource_path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QIcon();
  }
  QByteArray svg_data = file.readAll();
  file.close();
  recolorSvgInk(svg_data, pickerThemeIsLight());
  return rasterizeSvgIcon(svg_data, px);
}

// Render one of the app's Material chevron SVGs (keyboard_arrow_left/right),
// recolored to `color`, into a px*DPR QIcon. The source path's own fill is
// irrelevant — we always load the "_light" variant and swap its #3D3D3D fill for
// `color` (theme ink for the resting state, the from/to hint colors when active).
// Cached by (direction, color, px): updateNavButtons() re-applies icons on every
// hovered-date change, so without the cache each mouse move would re-parse + raster
// four SVGs.
QIcon renderChevronIcon(bool left, const QColor& color, int px) {
  static QHash<QString, QIcon> cache;
  // Key on HexRgb to match the recolor below (color.name() drops alpha), so the
  // key can never disagree with the rasterized output.
  const QString key = u"%1:%2:%3"_s.arg(left ? 1 : 0).arg(color.name()).arg(px);
  const auto cached = cache.constFind(key);
  if (cached != cache.constEnd()) {
    return cached.value();
  }

  const QString path =
      left ? u":/resources/svg/keyboard_arrow_left_light.svg"_s : u":/resources/svg/keyboard_arrow_right_light.svg"_s;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QIcon();
  }
  QByteArray svg_data = file.readAll();
  file.close();
  svg_data.replace("#3D3D3D", color.name().toUtf8());
  QIcon icon = rasterizeSvgIcon(svg_data, px);
  cache.insert(key, icon);
  return icon;
}

}  // namespace

// ===========================================================================
// CalendarWidget
// ===========================================================================

CalendarWidget::CalendarWidget(QWidget* parent)
    : QWidget(parent), year_(QDate::currentDate().year()), month_(QDate::currentDate().month()) {
  setMouseTracking(true);
  // Accept focus on click: otherwise a focused time spin box KEEPS focus (and
  // its editing caret) when the user clicks a date — sticky-focus complaint.
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(320, 230);  // weekday row + 6 date rows (no month title)
}

void CalendarWidget::setMonth(int year, int month) {
  year_ = year;
  month_ = month;
  update();
}

void CalendarWidget::setMediated(bool mediated) {
  mediated_ = mediated;
}

void CalendarWidget::setRange(const QDate& from, const QDate& to) {
  range_from_ = from;
  range_to_ = to;
  update();
}

void CalendarWidget::setHoverDate(const QDate& date) {
  hover_date_ = date;
  update();
}

void CalendarWidget::clearRange() {
  range_from_ = QDate();
  range_to_ = QDate();
  hover_date_ = QDate();
  state_ = State::kIdle;
  update();
}

int CalendarWidget::headerHeight() const {
  // Weekday-name row only; month/year lives in RangeCalendarWidget's header
  // controls, not the paint.
  return fontMetrics().height() + kHeaderPad * 2;
}

int CalendarWidget::firstDayColumn() const {
  int dow = QDate(year_, month_, 1).dayOfWeek();  // Qt: 1=Mon, 7=Sun
  return dow - 1;
}

QRect CalendarWidget::cellRect(int row, int col) const {
  int hh = headerHeight();
  int cell_w = width() / kCols;
  int cell_h = (height() - hh) / kRows;
  return QRect(col * cell_w, hh + row * cell_h, cell_w, cell_h);
}

QDate CalendarWidget::dateAtPosition(const QPoint& pos) const {
  int hh = headerHeight();
  if (pos.y() < hh) {
    return QDate();
  }
  int cell_w = width() / kCols;
  int cell_h = (height() - hh) / kRows;
  int col = pos.x() / cell_w;
  int row = (pos.y() - hh) / cell_h;
  if (col < 0 || col >= kCols || row < 0 || row >= kRows) {
    return QDate();
  }
  int day_index = row * kCols + col - firstDayColumn();
  int day = day_index + 1;
  if (day < 1 || day > QDate(year_, month_, 1).daysInMonth()) {
    return QDate();
  }
  return QDate(year_, month_, day);
}

void CalendarWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  // Source colors from the active theme's tokens instead of palette() roles:
  // the app pins QPalette at Fusion defaults regardless of the QSS theme, so
  // palette() would resolve to Fusion light-grey on both themes. pickerTokens()
  // picks the right light/dark shades off the persisted "StyleSheet::theme" key.
  const PickerTokens tok = pickerTokens();
  p.fillRect(rect(), tok.surface);

  int hh = headerHeight();
  int cell_w = width() / kCols;
  int fm_h = fontMetrics().height();

  // Day name headers only; the month/year lives in RangeCalendarWidget's header
  // controls, so this grid is intentionally title-less — do NOT paint an
  // "MMMM yyyy" title here (it would overdraw the weekday row / calendar).

  // Day name headers
  p.setFont(font());
  static const char* day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  int day_header_y = kHeaderPad / 2;
  QColor weekend_color(0xef, 0x53, 0x50);
  QColor header_color = tok.muted;
  for (int c = 0; c < kCols; ++c) {
    QRect r(c * cell_w, day_header_y, cell_w, fm_h + kHeaderPad);
    p.setPen((c >= 5) ? weekend_color : header_color);
    p.drawText(r, Qt::AlignCenter, day_names[c]);
  }

  // Effective range for painting
  QDate eff_from = range_from_;
  QDate eff_to = range_to_;
  if (eff_from.isValid() && !eff_to.isValid() && hover_date_.isValid()) {
    eff_to = hover_date_;
  }
  if (eff_from.isValid() && eff_to.isValid() && eff_from > eff_to) {
    std::swap(eff_from, eff_to);
  }

  int days_in_month = QDate(year_, month_, 1).daysInMonth();
  int first_col = firstDayColumn();

  QColor from_color(0x2e, 0xcc, 0x71);
  QColor to_color(0xe7, 0x4c, 0x3c);
  QColor range_color = tok.selection_range;
  range_color.setAlpha(60);
  QColor hover_brush = tok.hover_fill;
  QColor hover_border = tok.hover_border;

  for (int day = 1; day <= days_in_month; ++day) {
    int idx = (day - 1) + first_col;
    int row = idx / kCols;
    int col = idx % kCols;
    QRect cell = cellRect(row, col);
    QDate date(year_, month_, day);

    bool is_from = eff_from.isValid() && date == eff_from;
    bool is_to = eff_to.isValid() && date == eff_to;
    bool in_range = eff_from.isValid() && eff_to.isValid() && date > eff_from && date < eff_to;
    bool is_hovered = hover_date_.isValid() && date == hover_date_ && !is_from && !is_to && !in_range;

    QRect inner = cell.adjusted(1, 1, -1, -1);
    if (is_from) {
      p.setBrush(from_color);
      p.setPen(Qt::NoPen);
      p.drawRoundedRect(inner, 6, 6);
    } else if (is_to) {
      p.setBrush(to_color);
      p.setPen(Qt::NoPen);
      p.drawRoundedRect(inner, 6, 6);
    } else if (in_range) {
      p.setBrush(range_color);
      p.setPen(Qt::NoPen);
      p.drawRect(inner);
    } else if (is_hovered) {
      p.setBrush(hover_brush);
      p.setPen(QPen(hover_border, 1));
      p.drawRoundedRect(inner, 4, 4);
    }

    QColor text_color;
    if (is_from || is_to) {
      text_color = Qt::white;
    } else if (col >= 5) {
      text_color = weekend_color;
    } else {
      text_color = tok.text;
    }
    p.setPen(text_color);
    p.setFont(font());
    p.drawText(cell, Qt::AlignCenter, QString::number(day));
  }

  QColor grid_color = tok.border;
  grid_color.setAlpha(80);
  p.setPen(QPen(grid_color, 0.5));
  for (int r = 0; r <= kRows; ++r) {
    QRect cell = cellRect(r, 0);
    p.drawLine(0, cell.y(), width(), cell.y());
  }
  for (int c = 0; c <= kCols; ++c) {
    int x = c * cell_w;
    p.drawLine(x, hh, x, height());
  }
}

void CalendarWidget::mouseMoveEvent(QMouseEvent* event) {
  QDate date = dateAtPosition(event->pos());
  setCursor(date.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
  if (date.isValid()) {
    if (mediated_) {
      emit dateHovered(date);
    } else if (state_ == State::kSelecting) {
      hover_date_ = date;
      update();
    } else if (hover_date_ != date) {
      hover_date_ = date;
      update();
    }
  } else {
    if (mediated_) {
      emit hoverLeft();
    } else if (hover_date_.isValid()) {
      hover_date_ = QDate();
      update();
    }
  }
}

void CalendarWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  QDate date = dateAtPosition(event->pos());
  if (!date.isValid()) {
    return;
  }
  if (mediated_) {
    emit dateClicked(date);
    return;
  }
  switch (state_) {
    case State::kIdle:
      range_from_ = date;
      range_to_ = QDate();
      state_ = State::kSelecting;
      break;
    case State::kSelecting:
      range_to_ = date;
      if (range_from_ > range_to_) {
        std::swap(range_from_, range_to_);
      }
      hover_date_ = QDate();
      state_ = State::kCommitted;
      break;
    case State::kCommitted:
      if (qAbs(range_from_.daysTo(date)) <= qAbs(range_to_.daysTo(date))) {
        range_from_ = date;
      } else {
        range_to_ = date;
      }
      if (range_from_ > range_to_) {
        std::swap(range_from_, range_to_);
      }
      break;
  }
  update();
}

void CalendarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  QDate date = dateAtPosition(event->pos());
  if (!date.isValid()) {
    return;
  }
  if (mediated_) {
    emit dateDoubleClicked(date);
    return;
  }
  range_from_ = date;
  range_to_ = date;
  hover_date_ = QDate();
  state_ = State::kCommitted;
  update();
}

void CalendarWidget::leaveEvent(QEvent* event) {
  Q_UNUSED(event);
  if (mediated_) {
    emit hoverLeft();
  } else if (hover_date_.isValid()) {
    hover_date_ = QDate();
    update();
  }
}

// ===========================================================================
// TimePickerWidget
// ===========================================================================

TimePickerWidget::TimePickerWidget(QWidget* parent) : QWidget(parent) {
  // TWO rows ("From time:" / "To time:") so the captions + fields fit the
  // overlay width.
  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setHorizontalSpacing(6);
  grid->setVerticalSpacing(4);

  auto make_time_row = [&](int row, const QString& caption, QLabel*& date_label, IntScrubber*& hour,
                           IntScrubber*& minute) {
    auto* cap = new QLabel(caption);
    QFont cap_font = cap->font();
    cap_font.setBold(true);
    cap->setFont(cap_font);
    date_label = new QLabel("---");
    // App Blender-style scrubbers (drag / arrows / double-click edit) for the
    // native input chrome. HEIGHT comes from the app QSS (#PickerOverlay
    // PJ--ScrubberBase → 26px) so the rows match the dialog's other inputs.
    hour = new IntScrubber;
    hour->setRange(0, 23);  // 24-hour clock
    hour->setFixedWidth(56);
    auto* colon = new QLabel(":");
    minute = new IntScrubber;
    minute->setRange(0, 59);
    minute->setPadWidth(2);  // minutes always show two digits ("05", not "5")
    minute->setFixedWidth(56);
    grid->addWidget(cap, row, 0);
    grid->addWidget(date_label, row, 1);
    grid->addWidget(hour, row, 2);
    grid->addWidget(colon, row, 3);
    grid->addWidget(minute, row, 4);
  };
  grid->setColumnStretch(5, 1);  // trailing stretch keeps both rows left-packed

  make_time_row(0, tr("From time:"), from_date_label_, from_hour_, from_minute_);
  from_hour_->setValue(0);  // default full-day range: 00:00 ...
  from_minute_->setValue(0);

  make_time_row(1, tr("To time:"), to_date_label_, to_hour_, to_minute_);
  to_hour_->setValue(23);  // ... to 23:59
  to_minute_->setValue(59);

  // editingFinished (not valueChanged): it fires when a gesture SETTLES —
  // drag release, arrow/autorepeat burst end, committed edit — so the range
  // filter is not re-emitted on every intermediate scrub step.
  auto emit_changed = [this]() { emit timeChanged(); };
  connect(from_hour_, &ScrubberBase::editingFinished, this, emit_changed);
  connect(from_minute_, &ScrubberBase::editingFinished, this, emit_changed);
  connect(to_hour_, &ScrubberBase::editingFinished, this, emit_changed);
  connect(to_minute_, &ScrubberBase::editingFinished, this, emit_changed);
}

void TimePickerWidget::setFromDate(const QDate& date) {
  from_date_label_->setText(date.isValid() ? date.toString("dd-MM-yy") : u"---"_s);
}

void TimePickerWidget::setToDate(const QDate& date) {
  to_date_label_->setText(date.isValid() ? date.toString("dd-MM-yy") : u"---"_s);
}

QTime TimePickerWidget::fromTime() const {
  return QTime(from_hour_->value(), from_minute_->value());
}

QTime TimePickerWidget::toTime() const {
  return QTime(to_hour_->value(), to_minute_->value());
}

// ===========================================================================
// RangeCalendarWidget
// ===========================================================================

namespace {
// Signed month comparison. Returns 0 for an invalid date.
int monthCmp(const QDate& d, int year, int month) {
  if (!d.isValid()) {
    return 0;
  }
  if (d.year() != year) {
    return (d.year() < year) ? -1 : +1;
  }
  if (d.month() != month) {
    return (d.month() < month) ? -1 : +1;
  }
  return 0;
}
}  // namespace

void RangeCalendarWidget::advanceMonth(int& year, int& month, int delta) {
  month += delta;
  while (month > 12) {
    month -= 12;
    year++;
  }
  while (month < 1) {
    month += 12;
    year--;
  }
}

RangeCalendarWidget::RangeCalendarWidget(QWidget* parent)
    : QWidget(parent), year_(QDate::currentDate().year()), month_(QDate::currentDate().month()) {
  max_year_ = year_;
  min_year_ = max_year_ - 6;  // widened by setYearSpan / on-demand by setMonth

  // Flat icon buttons carrying the big themed chevrons (28x28 to match the rest
  // of the chrome's icon buttons); syncHeaderControls() re-inks them per the
  // direction-hint state.
  const QSize btn_size(28, 28);
  const QColor ink = pickerTokens().text;
  auto setup_nav = [&](QPushButton* b, bool left) {
    b->setIcon(renderChevronIcon(left, ink, kNavChevronPx));
    b->setIconSize(QSize(kNavChevronPx, kNavChevronPx));
    b->setFixedSize(btn_size);
    b->setFlat(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(u"QPushButton { border: none; background: transparent; padding: 0; }"_s);
  };
  prev_ = new QPushButton;
  next_ = new QPushButton;
  setup_nav(prev_, /*left=*/true);
  setup_nav(next_, /*left=*/false);

  // Month/year jump combos: the data spans years, so direct jumps beat chevron
  // marching.
  month_combo_ = new ComboBox;
  month_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  for (int m = 1; m <= 12; ++m) {
    month_combo_->addItem(QLocale().standaloneMonthName(m), m);
  }
  year_combo_ = new ComboBox;
  year_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);

  auto* header = new QHBoxLayout;
  header->setContentsMargins(0, 0, 0, 0);
  header->setSpacing(4);
  header->addWidget(prev_);
  header->addStretch();
  header->addWidget(month_combo_);
  header->addWidget(year_combo_);
  header->addStretch();
  header->addWidget(next_);

  calendar_ = new CalendarWidget;
  calendar_->setMediated(true);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(8, 8, 8, 8);
  main_layout->setSpacing(4);
  main_layout->addLayout(header);
  main_layout->addWidget(calendar_);

  connect(prev_, &QPushButton::clicked, this, &RangeCalendarWidget::prevMonth);
  connect(next_, &QPushButton::clicked, this, &RangeCalendarWidget::nextMonth);
  connect(month_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    if (idx >= 0) {
      setMonth(year_, idx + 1);
    }
  });
  connect(year_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    const int y = year_combo_->currentData().toInt();
    if (y > 0) {
      setMonth(y, month_);
    }
  });
  connect(calendar_, &CalendarWidget::dateClicked, this, &RangeCalendarWidget::onDateClicked);
  connect(calendar_, &CalendarWidget::dateHovered, this, &RangeCalendarWidget::onDateHovered);
  connect(calendar_, &CalendarWidget::hoverLeft, this, &RangeCalendarWidget::onHoverLeft);

  setMonth(year_, month_);
}

void RangeCalendarWidget::setYearSpan(const QDate& earliest, const QDate& latest) {
  const int cur = QDate::currentDate().year();
  const int hi = latest.isValid() ? std::max(latest.year(), cur) : cur;
  const int lo = earliest.isValid() ? std::min(earliest.year(), hi) : hi - 6;
  if (lo == min_year_ && hi == max_year_) {
    return;
  }
  min_year_ = lo;
  max_year_ = hi;
  syncHeaderControls();
}

void RangeCalendarWidget::setMonth(int year, int month) {
  // Never refuse to show a real date: widen the year span on demand instead of
  // clamping (a persisted from/to can predate the current data hints).
  min_year_ = std::min(min_year_, year);
  max_year_ = std::max(max_year_, year);
  year_ = year;
  month_ = std::clamp(month, 1, 12);
  calendar_->setMonth(year_, month_);
  syncHeaderControls();
  broadcastState();
}

void RangeCalendarWidget::setExternalRange(const QDate& from, const QDate& to) {
  const QDate new_from = from.isValid() ? from : QDate();
  const QDate new_to = to.isValid() ? to : QDate();
  // Jump the view to the range start (or the end when only that is known).
  int ny = year_;
  int nm = month_;
  if (new_from.isValid()) {
    ny = new_from.year();
    nm = new_from.month();
  } else if (new_to.isValid()) {
    ny = new_to.year();
    nm = new_to.month();
  }
  if (ny == year_ && nm == month_ && new_from == range_from_ && new_to == range_to_ && !selecting_) {
    return;
  }
  range_from_ = new_from;
  range_to_ = new_to;
  hover_date_ = QDate();
  selecting_ = false;
  setMonth(ny, nm);  // syncs the header + broadcasts the range to the grid
}

void RangeCalendarWidget::retheme() {
  // Reset the hint gate: the signature is unchanged on a theme toggle but the
  // resting INK color is not — without this the skip keeps the old theme's ink.
  last_hint_sig_ = -1;
  syncHeaderControls();  // re-inks the chevrons via renderChevronIcon(pickerTokens().text)
  calendar_->update();
}

void RangeCalendarWidget::prevMonth() {
  int y = year_;
  int m = month_;
  advanceMonth(y, m, -1);
  setMonth(y, m);
}

void RangeCalendarWidget::nextMonth() {
  int y = year_;
  int m = month_;
  advanceMonth(y, m, +1);
  setMonth(y, m);
}

void RangeCalendarWidget::syncHeaderControls() {
  // (Re)fill the year combo when the span changed.
  const int count = max_year_ - min_year_ + 1;
  const bool refill =
      year_combo_->count() != count || (year_combo_->count() > 0 && year_combo_->itemData(0).toInt() != min_year_);
  if (refill) {
    const QSignalBlocker block(year_combo_);
    year_combo_->clear();
    for (int y = min_year_; y <= max_year_; ++y) {
      year_combo_->addItem(QString::number(y), y);
    }
  }
  {
    const QSignalBlocker bm(month_combo_);
    month_combo_->setCurrentIndex(month_ - 1);
    const QSignalBlocker by(year_combo_);
    const int idx = year_combo_->findData(year_);
    if (idx >= 0) {
      year_combo_->setCurrentIndex(idx);
    }
  }

  // Direction hint: color a chevron green/red when clicking it would move
  // toward the range start/end (currently outside the visible month); resting
  // state is the theme ink. Skip the setIcon calls when nothing changed —
  // broadcastState runs on every hover move, and an unconditional setIcon
  // repaints both buttons per mouse move for no visual difference.
  const QColor ink = pickerTokens().text;
  const int from_cmp = monthCmp(range_from_, year_, month_);
  const int to_cmp = monthCmp(range_to_, year_, month_);
  const int hint_sig = (from_cmp < 0) | ((to_cmp < 0) << 1) | ((from_cmp > 0) << 2) | ((to_cmp > 0) << 3);
  if (hint_sig == last_hint_sig_) {
    return;
  }
  last_hint_sig_ = hint_sig;
  auto apply_hint = [ink](QPushButton* btn, bool left, bool hint_from, bool hint_to) {
    const QColor c = hint_from ? kFromHintColor : (hint_to ? kToHintColor : ink);
    btn->setIcon(renderChevronIcon(left, c, kNavChevronPx));
  };
  apply_hint(prev_, /*left=*/true, from_cmp < 0, to_cmp < 0);
  apply_hint(next_, /*left=*/false, from_cmp > 0, to_cmp > 0);
}

void RangeCalendarWidget::onDateClicked(const QDate& date) {
  if (!selecting_) {
    range_from_ = date;
    range_to_ = QDate();
    selecting_ = true;
  } else {
    range_to_ = date;
    if (range_from_ > range_to_) {
      std::swap(range_from_, range_to_);
    }
    hover_date_ = QDate();
    selecting_ = false;
    emit rangeCommitted(range_from_, range_to_);
  }
  broadcastState();
}

void RangeCalendarWidget::onDateHovered(const QDate& date) {
  hover_date_ = date;
  broadcastState();
}

void RangeCalendarWidget::onHoverLeft() {
  if (hover_date_.isValid()) {
    hover_date_ = QDate();
    broadcastState();
  }
}

void RangeCalendarWidget::broadcastState() {
  QDate eff_from = range_from_;
  QDate eff_to = range_to_;
  if (selecting_ && hover_date_.isValid()) {
    eff_to = hover_date_;
  }
  if (eff_from.isValid() && eff_to.isValid() && eff_from > eff_to) {
    std::swap(eff_from, eff_to);
  }
  calendar_->setRange(eff_from, eff_to);
  calendar_->setHoverDate(hover_date_);
  syncHeaderControls();
  if (selecting_ && eff_from.isValid() && eff_to.isValid()) {
    emit rangePreview(eff_from, eff_to);
  }
}

// ===========================================================================
// DateRangePicker
// ===========================================================================

DateRangePicker::DateRangePicker(QWidget* parent) : QWidget(parent) {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  // Match the host column's 2px row spacing: without this the preset-row -> date-row
  // gap uses Qt's larger default, so the spacing between the preset buttons and the
  // from/to fields reads bigger than the gaps to the header above / table below.
  main_layout->setSpacing(2);

  auto* preset_row = new QHBoxLayout;
  preset_group_ = new QButtonGroup(this);
  preset_group_->setExclusive(true);

  // Fixed-width, left-aligned preset buttons (they used to stretch across the
  // panel); "Custom" is its own button leading the from/to row below, aligned
  // under "All", instead of the old shape-shifting All/Custom label.
  constexpr int kPresetButtonWidth = 100;
  auto make_preset = [&](int id, const QString& text) -> QPushButton* {
    auto* btn = new QPushButton(text);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedWidth(kPresetButtonWidth);
    preset_group_->addButton(btn, id);
    return btn;
  };
  auto add_preset = [&](int id, const QString& text) -> QPushButton* {
    auto* btn = make_preset(id, text);
    preset_row->addWidget(btn);
    return btn;
  };

  all_button_ = add_preset(kPresetAll, "All");
  add_preset(kPresetPast24h, "Past 24h");
  add_preset(kPresetLast7Days, "Last 7 Days");
  add_preset(kPresetLastMonth, "Last Month");
  preset_row->addStretch();
  all_button_->setChecked(true);
  connect(preset_group_, &QButtonGroup::idClicked, this, &DateRangePicker::onPresetClicked);
  main_layout->addLayout(preset_row);

  auto* date_row = new QHBoxLayout;
  custom_button_ = make_preset(kPresetCustom, "Custom");
  custom_button_->setToolTip(tr("Pick a custom date range"));
  date_row->addWidget(custom_button_);
  from_edit_ = new QLineEdit;
  from_edit_->setPlaceholderText("DD/MM/YYYY");
  // Material "Arrow Right Alt" between from/to, themed to the active ink (re-inked
  // on a live theme switch in changeEvent). A QLabel pixmap, not a text glyph.
  arrow_label_ = new QLabel;
  arrow_label_->setPixmap(renderThemedIcon(u":/resources/svg/arrow_right_alt.svg"_s, 18).pixmap(18, 18));
  to_edit_ = new QLineEdit;
  to_edit_->setPlaceholderText(QDate::currentDate().toString("dd/MM/yyyy"));
  calendar_button_ = new QPushButton;
  calendar_button_->setCursor(Qt::PointingHandCursor);
  // 28x28 button with a 24px icon to match the panel's other icon buttons
  // (refresh / search / regex toggles).
  calendar_button_->setFixedSize(28, 28);
  // Themed "Calendar Month" icon (recolored to the active theme's ink); the
  // icon stays static — toggleCalendar() no longer swaps a glyph.
  calendar_button_->setIcon(renderThemedIcon(u":/resources/svg/calendar_month.svg"_s, 24));
  calendar_button_->setIconSize(QSize(24, 24));
  date_row->addWidget(from_edit_, 1);
  date_row->addWidget(arrow_label_);
  date_row->addWidget(to_edit_, 1);
  date_row->addWidget(calendar_button_);
  connect(from_edit_, &QLineEdit::textChanged, this, &DateRangePicker::checkCustomState);
  connect(to_edit_, &QLineEdit::textChanged, this, &DateRangePicker::checkCustomState);
  connect(calendar_button_, &QPushButton::clicked, this, &DateRangePicker::toggleCalendar);
  main_layout->addLayout(date_row);
}

DateRangePicker::~DateRangePicker() {
  delete overlay_;
}

void DateRangePicker::setEarliestDate(const QDate& date) {
  from_edit_->setPlaceholderText(date.isValid() ? date.toString("dd/MM/yyyy") : u"DD/MM/YYYY"_s);
  earliest_hint_ = date;
  if (range_calendar_) {
    range_calendar_->setYearSpan(earliest_hint_, latest_hint_);
  }
}

void DateRangePicker::setLatestDate(const QDate& date) {
  to_edit_->setPlaceholderText(date.isValid() ? date.toString("dd/MM/yyyy") : u"DD/MM/YYYY"_s);
  latest_hint_ = date;
  if (range_calendar_) {
    range_calendar_->setYearSpan(earliest_hint_, latest_hint_);
  }
}

void DateRangePicker::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Default the "to" placeholder to today only when no latest hint was pushed.
  if (to_edit_->placeholderText().isEmpty()) {
    to_edit_->setPlaceholderText(QDate::currentDate().toString("dd/MM/yyyy"));
  }

  if (!overlay_ && window()) {
    overlay_ = new QWidget(window());
    overlay_->setObjectName("PickerOverlay");
    // Clicks on the overlay's dead space (captions, margins) must release the
    // focus a time spin box is holding — see CalendarWidget's ClickFocus twin.
    overlay_->setFocusPolicy(Qt::ClickFocus);
    // Drop shadow so the popup visibly floats over the busy tables beneath it.
    // It re-renders the overlay per repaint, which is cheap (hover repaints
    // touch only the calendar grid).
    auto* shadow = new QGraphicsDropShadowEffect(overlay_);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    shadow->setColor(QColor(0, 0, 0, 90));
    overlay_->setGraphicsEffect(shadow);
    updateOverlayStyle();
    auto* overlay_layout = new QVBoxLayout(overlay_);
    range_calendar_ = new RangeCalendarWidget;
    range_calendar_->setYearSpan(earliest_hint_, latest_hint_);
    overlay_layout->addWidget(range_calendar_);
    time_picker_ = new TimePickerWidget;
    overlay_layout->addWidget(time_picker_);
    // Explicit dismiss affordance: an app-consistent X in the overlay's
    // top-right corner (close-button.svg — the same glyph the app's other
    // dismissables use). Selections apply live, so it just hides the overlay.
    // It floats above the layout (positioned in repositionOverlay); the
    // layout's top margin reserves its strip so nothing collides with it.
    overlay_layout->setContentsMargins(9, 26, 9, 9);
    overlay_close_ = new SvgButton(u":/resources/svg/close-button.svg"_s, SvgButton::Size::kDefault, overlay_);
    overlay_close_->setObjectName("PickerCloseX");
    overlay_close_->setExtent(20, 16);
    overlay_close_->setCursor(Qt::PointingHandCursor);
    overlay_close_->setToolTip(tr("Close"));
    connect(overlay_close_, &SvgButton::clicked, this, [this]() {
      calendar_visible_ = false;
      if (overlay_) {
        overlay_->setVisible(false);
      }
    });
    overlay_->setVisible(false);
    connect(range_calendar_, &RangeCalendarWidget::rangeCommitted, this, &DateRangePicker::onCalendarRangeCommitted);
    connect(range_calendar_, &RangeCalendarWidget::rangePreview, this, &DateRangePicker::onCalendarRangePreview);
    connect(time_picker_, &TimePickerWidget::timeChanged, this, &DateRangePicker::onTimeChanged);
    window()->installEventFilter(this);
  }
}

void DateRangePicker::changeEvent(QEvent* event) {
  // Re-theme on a live theme toggle. Both StyleChange and PaletteChange are
  // handled: the app pins QPalette but a theme swap can still arrive as either,
  // so cover both. The calendars/overlay paint from pickerTokens() (keyed off
  // the persisted theme), so we re-apply the overlay stylesheet, re-render the
  // button icon in the new ink, and force a repaint of the overlay subtree
  // (it lives under window(), not under this widget, so it won't repaint on its
  // own from a change event delivered here).
  if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
    updateOverlayStyle();
    if (calendar_button_) {
      calendar_button_->setIcon(renderThemedIcon(u":/resources/svg/calendar_month.svg"_s, 24));
    }
    if (arrow_label_) {
      arrow_label_->setPixmap(renderThemedIcon(u":/resources/svg/arrow_right_alt.svg"_s, 18).pixmap(18, 18));
    }
    if (range_calendar_) {
      range_calendar_->retheme();  // re-ink the nav chevrons to the new theme
    }
    if (overlay_) {
      const auto children = overlay_->findChildren<QWidget*>();
      overlay_->update();
      for (auto* child : children) {
        child->update();
      }
    }
  }
  QWidget::changeEvent(event);
}

void DateRangePicker::updateOverlayStyle() {
  if (!overlay_) {
    return;
  }
  // Drive the overlay popup from the theme tokens so it matches the active QSS
  // theme (background = dark_background, 1px solid border_default), with
  // square corners. palette()
  // can't be used here: the app pins QPalette at Fusion defaults regardless of
  // theme, so it would resolve to Fusion light-grey on both.
  const PickerTokens tok = pickerTokens();
  overlay_->setStyleSheet(u"QWidget#PickerOverlay { background-color: %1; border: 1px solid %2; }"_s.arg(
      tok.surface.name(), tok.border.name()));
}

void DateRangePicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (overlay_ && overlay_->isVisible()) {
    repositionOverlay();
  }
}

bool DateRangePicker::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window() && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
    if (overlay_ && overlay_->isVisible()) {
      repositionOverlay();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void DateRangePicker::repositionOverlay() {
  if (!overlay_ || !calendar_button_) {
    return;
  }
  // Force a layout pass BEFORE reading sizeHint: on the very first open the
  // overlay's layout hasn't activated yet, so the hint under-reports and the
  // grid/time row came up clipped until the next reposition.
  if (overlay_->layout()) {
    overlay_->layout()->activate();
  }
  overlay_->resize(overlay_->sizeHint());
  // Anchor the overlay's top-RIGHT corner under the calendar button (a
  // dropdown hangs off the control that opened it), clamped to the window.
  QPoint anchor = calendar_button_->mapTo(window(), QPoint(calendar_button_->width(), calendar_button_->height()));
  int x = anchor.x() - overlay_->width();
  int y = anchor.y() + 2;
  x = std::max(4, std::min(x, window()->width() - overlay_->width() - 4));
  overlay_->move(x, y);
  overlay_->raise();
  if (overlay_close_) {
    // Corner X lives in the strip the overlay layout's top margin reserves.
    overlay_close_->move(overlay_->width() - overlay_close_->width() - 6, 4);
    overlay_close_->raise();
  }
}

void DateRangePicker::onPresetClicked(int id) {
  if (id == kPresetCustom) {
    // Custom is an invitation, not a window: keep the fields as they are and
    // open the calendar so the user can define the range.
    if (!calendar_visible_) {
      toggleCalendar();
    }
    return;
  }
  applyPreset(id);
  syncCalendarToFields();
  emitFilter();
}

void DateRangePicker::syncCalendarToFields() {
  if (!range_calendar_) {
    return;
  }
  const QString fmt = u"dd/MM/yyyy"_s;
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  range_calendar_->setExternalRange(from, to);
}

void DateRangePicker::applyPreset(int id) {
  QDate today = QDate::currentDate();
  QDate from, to;
  switch (id) {
    case kPresetAll:
      break;
    case kPresetPast24h:
      from = today.addDays(-1);
      to = today;
      break;
    case kPresetLast7Days:
      from = today.addDays(-6);
      to = today;
      break;
    case kPresetLastMonth: {
      QDate first_of_month(today.year(), today.month(), 1);
      to = first_of_month.addDays(-1);
      from = QDate(to.year(), to.month(), 1);
      break;
    }
    case kPresetCustom:
      return;  // never a window of its own — fields stay as the user set them
  }
  updateFieldsFromPreset(from, to);
}

void DateRangePicker::updateFieldsFromPreset(const QDate& from, const QDate& to) {
  const QString fmt = u"dd/MM/yyyy"_s;
  QSignalBlocker fb(from_edit_);
  QSignalBlocker tb(to_edit_);
  from_edit_->setText(from.isValid() ? from.toString(fmt) : QString());
  to_edit_->setText(to.isValid() ? to.toString(fmt) : QString());
}

int DateRangePicker::matchingPreset() const {
  const QString fmt = u"dd/MM/yyyy"_s;
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  QDate today = QDate::currentDate();

  if (from_edit_->text().isEmpty() && to_edit_->text().isEmpty()) {
    return kPresetAll;
  }
  if (!from.isValid() || !to.isValid()) {
    return -1;
  }
  if (from == today.addDays(-1) && to == today) {
    return kPresetPast24h;
  }
  if (from == today.addDays(-6) && to == today) {
    return kPresetLast7Days;
  }
  QDate first_of_month(today.year(), today.month(), 1);
  QDate last_of_prev = first_of_month.addDays(-1);
  QDate first_of_prev(last_of_prev.year(), last_of_prev.month(), 1);
  if (from == first_of_prev && to == last_of_prev) {
    return kPresetLastMonth;
  }
  return -1;
}

void DateRangePicker::checkCustomState() {
  int preset = matchingPreset();
  if (preset >= 0) {
    preset_group_->button(preset)->setChecked(true);
  } else {
    custom_button_->setChecked(true);
  }
  syncCalendarToFields();
  emitFilter();
}

void DateRangePicker::toggleCalendar() {
  if (!overlay_) {
    return;
  }
  calendar_visible_ = !calendar_visible_;
  overlay_->setVisible(calendar_visible_);
  // Icon stays static (no glyph swap) — the open/closed state is conveyed by
  // the overlay's visibility.
  if (calendar_visible_) {
    repositionOverlay();
  }
}

void DateRangePicker::onCalendarRangeCommitted(const QDate& from, const QDate& to) {
  updateFieldsFromPreset(from, to);
  checkCustomState();
  if (time_picker_) {
    time_picker_->setFromDate(from);
    time_picker_->setToDate(to);
  }
}

void DateRangePicker::onCalendarRangePreview(const QDate& from, const QDate& to) {
  // Preview updates the from/to FIELDS only. It must NOT emit the filter:
  // filterChanged reaches the plugin as dateRangeChanged, which invalidates its
  // sequence view — a full re-filter + table re-delivery of the whole catalog
  // (24k rows) PER MOUSE MOVE, throttling the hover preview to ~1-2 Hz. The
  // filter goes out once, on commit (onCalendarRangeCommitted).
  updateFieldsFromPreset(from, to);
}

void DateRangePicker::onTimeChanged() {
  emitFilter();
}

RangeFilter DateRangePicker::buildFilter() const {
  RangeFilter f;
  const QString fmt = u"dd/MM/yyyy"_s;
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  if (from.isValid()) {
    f.date_from = from;
  }
  if (to.isValid()) {
    f.date_to = to;
  }
  if (time_picker_) {
    f.from_time = time_picker_->fromTime();
    QTime to_t = time_picker_->toTime();
    f.to_time = QTime(to_t.hour(), to_t.minute(), 59, 999);
  }
  return f;
}

void DateRangePicker::emitFilter() {
  emit filterChanged(buildFilter());
}

}  // namespace PJ
