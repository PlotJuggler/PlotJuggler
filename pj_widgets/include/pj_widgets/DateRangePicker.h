#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
// Date/time range picker bundle, registered with the host's PjUiLoader so a plugin .ui can declare "DateRangePicker" by
// class name:
//   CalendarWidget       a painted month grid (title-less; the range widget's header row carries month, year, nav)
//   TimePickerWidget     from and to 24-hour time
//   RangeCalendarWidget  one calendar plus a [<][Month][Year][>] header row
//   DateRangePicker      inline presets, from/to fields and a calendar overlay
// DateRangePicker emits filterChanged(RangeFilter); the host binding serializes it into a dateRangeChanged event.

#include <QDate>
#include <QList>
#include <QTime>
#include <QWidget>
#include <optional>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace PJ {

class IntScrubber;
class SvgButton;

// --- CalendarWidget ---

class CalendarWidget : public QWidget {
  Q_OBJECT
 public:
  explicit CalendarWidget(QWidget* parent = nullptr);

  void setMonth(int year, int month);
  int year() const {
    return year_;
  }
  int month() const {
    return month_;
  }

  void setMediated(bool mediated);
  bool isMediated() const {
    return mediated_;
  }

  void setRange(const QDate& from, const QDate& to);
  void setHoverDate(const QDate& date);
  void clearRange();

  QDate rangeFrom() const {
    return range_from_;
  }
  QDate rangeTo() const {
    return range_to_;
  }

 signals:
  void dateClicked(const QDate& date);
  void dateDoubleClicked(const QDate& date);
  void dateHovered(const QDate& date);
  void hoverLeft();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  QDate dateAtPosition(const QPoint& pos) const;
  QRect cellRect(int row, int col) const;
  int headerHeight() const;
  int firstDayColumn() const;

  int year_;
  int month_;
  QDate range_from_;
  QDate range_to_;
  QDate hover_date_;

  enum class State { kIdle, kSelecting, kCommitted };
  State state_ = State::kIdle;
  bool mediated_ = false;
};

// --- TimePickerWidget ---

class TimePickerWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimePickerWidget(QWidget* parent = nullptr);

  void setFromDate(const QDate& date);
  void setToDate(const QDate& date);

  QTime fromTime() const;
  QTime toTime() const;

 signals:
  void timeChanged();

 private:
  QLabel* from_date_label_;
  IntScrubber* from_hour_;
  IntScrubber* from_minute_;
  QLabel* to_date_label_;
  IntScrubber* to_hour_;
  IntScrubber* to_minute_;
};

// --- RangeCalendarWidget ---
// One month grid plus a [<][Month][Year][>] header row. Two-click range selection with hover preview (click the start,
// click the end; a new click restarts). The month and year combos jump anywhere in the data's multi-year span, which
// chevron marching cannot.
class RangeCalendarWidget : public QWidget {
  Q_OBJECT
 public:
  explicit RangeCalendarWidget(QWidget* parent = nullptr);

  void setExternalRange(const QDate& from, const QDate& to);

  // Bounds of the year combo (from the picker's earliest and latest data hints); invalid dates fall back to [current-6,
  // current].
  void setYearSpan(const QDate& earliest, const QDate& latest);

  // Re-renders the nav chevrons and repaints for the active theme. DateRangePicker calls it on a live theme toggle: the
  // icons are baked, so a plain repaint would not re-ink them.
  void retheme();

 signals:
  void rangeCommitted(const QDate& from, const QDate& to);
  void rangePreview(const QDate& from, const QDate& to);

 private slots:
  void onDateClicked(const QDate& date);
  void onDateHovered(const QDate& date);
  void onHoverLeft();
  void prevMonth();
  void nextMonth();

 private:
  void setMonth(int year, int month);
  void syncHeaderControls();  // combos + chevron direction hints
  void broadcastState();
  static void advanceMonth(int& year, int& month, int delta);

  CalendarWidget* calendar_ = nullptr;
  QPushButton* prev_ = nullptr;
  QPushButton* next_ = nullptr;
  QComboBox* month_combo_ = nullptr;
  QComboBox* year_combo_ = nullptr;

  int year_;
  int month_;
  int min_year_ = 0;  // year-combo span (inclusive)
  int max_year_ = 0;
  // The last chevron hint signature applied by syncHeaderControls (-1: never, so the first sync always inks the
  // buttons). Per instance, so a rebuilt overlay does not inherit a stale skip.
  int last_hint_sig_ = -1;

  bool selecting_ = false;
  QDate range_from_;
  QDate range_to_;
  QDate hover_date_;
};

// --- RangeFilter and DateRangePicker ---

struct RangeFilter {
  std::optional<QDate> date_from;
  std::optional<QDate> date_to;
  QTime from_time = QTime(0, 0);
  QTime to_time = QTime(23, 59, 59, 999);
};

class DateRangePicker : public QWidget {
  Q_OBJECT
 public:
  explicit DateRangePicker(QWidget* parent = nullptr);
  ~DateRangePicker() override;

  void setEarliestDate(const QDate& date);
  void setLatestDate(const QDate& date);

 signals:
  void filterChanged(const RangeFilter& filter);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onPresetClicked(int id);
  void checkCustomState();
  void toggleCalendar();
  void onCalendarRangeCommitted(const QDate& from, const QDate& to);
  void onCalendarRangePreview(const QDate& from, const QDate& to);
  void onTimeChanged();

 private:
  void emitFilter();
  void repositionOverlay();
  void updateOverlayStyle();
  RangeFilter buildFilter() const;
  void applyPreset(int id);
  void updateFieldsFromPreset(const QDate& from, const QDate& to);
  int matchingPreset() const;
  void syncCalendarToFields();

  static constexpr int kPresetAll = 0;
  static constexpr int kPresetPast24h = 1;
  static constexpr int kPresetLast7Days = 2;
  static constexpr int kPresetLastMonth = 3;
  // Not a date window: checked when the fields hold a range no preset matches; clicking it opens the calendar overlay
  // to define one.
  static constexpr int kPresetCustom = 4;

  QButtonGroup* preset_group_ = nullptr;
  QPushButton* all_button_ = nullptr;
  QPushButton* custom_button_ = nullptr;  // leads the from/to row (kPresetCustom)
  QLineEdit* from_edit_ = nullptr;
  QLineEdit* to_edit_ = nullptr;
  QLabel* arrow_label_ = nullptr;  // Material arrow between from/to; re-inked on theme switch.
  QPushButton* calendar_button_ = nullptr;

  QWidget* overlay_ = nullptr;
  SvgButton* overlay_close_ = nullptr;  // corner X, positioned in repositionOverlay
  RangeCalendarWidget* range_calendar_ = nullptr;
  TimePickerWidget* time_picker_ = nullptr;
  // Data-span hints (setEarliestDate / setLatestDate), forwarded to the range calendar's year combo and retained
  // because the overlay is built lazily.
  QDate earliest_hint_;
  QDate latest_hint_;

  bool calendar_visible_ = false;
};

}  // namespace PJ
