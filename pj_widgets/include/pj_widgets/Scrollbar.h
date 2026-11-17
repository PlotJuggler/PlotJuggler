// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QPointer>
#include <QWidget>
#include <cstdint>
#include <functional>

#include "pj_widgets/FrameworkTokens.h"

class QAbstractScrollArea;
class QGraphicsOpacityEffect;
class QHeaderView;
class QPropertyAnimation;
class QScrollBar;
class QTimer;

namespace PJ::scrollbar_detail {

inline constexpr double kMinPillLenPx = 28.0;
inline constexpr double kPillThicknessPx = 6.0;
/// Width of the edge strip (px) the overlay widget occupies.
inline constexpr double kHoverStripPx = 14.0;
/// How long (ms) the pill stays revealed after the last scroll (wheel or gesture) event before it fades out, unless the
/// cursor then hovers the edge strip, in which case the hover path keeps it shown.
inline constexpr int kScrollRevealHoldMs = 900;

/// Picks the default pill accent for the theme identified by `window_color` (QPalette::Window); pure, so the choice is
/// unit-testable without a live widget.
[[nodiscard]] QColor defaultAccent(const QColor& window_color);

/// Handle geometry along the scroll axis: `pos` is the start offset (px) from the track origin, `len` the pill length
/// (px). Both are zero when there is nothing to scroll.
struct Handle {
  double pos = 0.0;
  double len = 0.0;
};

/// Maps scroll state to a pill handle along a track of `track_len_px`; {0,0} when there is nothing to scroll (max <=
/// min).
[[nodiscard]] Handle computeHandle(long min, long max, long value, long page_step, double track_len_px);

/// Maps a drag delta in viewport pixels to a native scroll bar value: `start_value + llround((delta_px / track_len_px)
/// * total)` with `total = (bar->maximum() - bar->minimum()) + bar->pageStep()`. Returns `start_value` unchanged when
/// `track_len_px <= 0`.
[[nodiscard]] long valueForDrag(long start_value, double delta_px, double track_len_px, long total);

}  // namespace PJ::scrollbar_detail

namespace PJ {

/// Scroll-pill widget that attaches to a QAbstractScrollArea and mirrors its native scroll bar as a thin fading pill,
/// in one of two placements:
///
///   - Placement::kOverlayViewport: the native bar policy is forced to ScrollBarAlwaysOff (the bar stays live as the
///     scroll-state source; only its visual widget is hidden) and this widget floats over the viewport's edge strip.
///     Content reflows under the strip, so the pill can cover it: acceptable only when the host owns the viewport and
///     keeps that strip free of interactive content (the Timeline).
///   - Placement::kReservedGutter: the native bar keeps its authored policy and layout slot (content never flows under
///     the pill) but is painted blank; this widget covers the bar's rect and mirrors its style-resolved handle. All
///     presses, drags and paging go to the native bar itself: the pill never consumes a mouse event, so it cannot steal
///     a content click. The default for areas adapted by attachPillScrollbars().
///
/// Common to both:
///   - The widget re-parents itself to the area and repaints whenever the native bar's value or range changes.
///   - A QGraphicsOpacityEffect fades the pill; it is hidden (opacity 0) by default and revealed when the cursor enters
///     the edge strip or gutter (auto-hide mode). setAutoHide(false) pins it visible.
///   - Scrolling the viewport (wheel or trackpad gesture) also reveals the matching-axis pill briefly, even with the
///     cursor nowhere near the strip; it fades kScrollRevealHoldMs after the last scroll unless the cursor then hovers
///     the strip.
///
/// Overlay-mode drag-to-scroll: a left press on the handle grabs the pill (with setClickToScroll(true), anywhere in the
/// strip). Events during an active drag are consumed (return true), so the underlying view is not affected; hover
/// events (no drag) are observed only.
class Scrollbar : public QWidget {
  Q_OBJECT

 public:
  /// Where the pill lives relative to the scroll area's layout (see the class comment).
  enum class Placement : std::uint8_t {
    kOverlayViewport,
    kReservedGutter,
  };

  explicit Scrollbar(Qt::Orientation orientation, QWidget* parent = nullptr);

  /// Attaches to a scroll area; once only, re-attaching is not supported. Afterwards the widget covers the viewport's
  /// edge strip (overlay) or the native bar's gutter (reserved gutter) and fades in on hover (auto-hide on by default).
  /// Enables mouse tracking on the area's viewport, so hover MouseMoves are delivered: without it the pill would appear
  /// only on click and never re-hide on motion inside the viewport. Reserved-gutter mode leaves the axis policy
  /// untouched: attaching to an axis pinned ScrollBarAlwaysOff yields a permanently empty pill (the bar never shows),
  /// so callers skip such axes (attachPillScrollbars does).
  void attach(QAbstractScrollArea* area, Placement placement = Placement::kOverlayViewport);

  /// The placement chosen at attach() time; a test hook.
  [[nodiscard]] Placement placement() const {
    return placement_;
  }

  [[nodiscard]] Qt::Orientation orientation() const {
    return orientation_;
  }

  /// Overrides the pill's accent color. By default it picks a theme-aware accent through
  /// scrollbar_detail::defaultAccent() (info-blue on a light theme, pale grey on a dark one), re-evaluated on theme
  /// changes; call this only for an explicit per-instance color, which pins it and opts out of theme tracking.
  void setAccentColor(const QColor& color);

  /// Enables or disables auto-hide (default true). False forces the pill to full opacity at once and keeps it visible
  /// wherever the cursor is. True resumes hover and fade (opacity is not changed at once: the next MouseMove or Leave
  /// on the viewport drives it).
  void setAutoHide(bool auto_hide);

  /// Sets the fade animation duration in ms (default 150); 0 snaps to the target opacity. Safe before or after
  /// attach().
  void setFadeDurationMs(int ms);

  /// The current rendered opacity [0.0, 1.0]; a test hook.
  double currentOpacity() const;

  /// Whether the pill is logically shown (the fade's end target): true at once after setShown(true), whatever the
  /// animation progress. A test hook.
  bool isShown() const {
    return shown_;
  }

  /// The current fade animation duration in ms; a test hook.
  int fadeDurationMs() const {
    return fade_ms_;
  }

  /// The mirrored handle's start offset and length along the scroll axis, in this widget's coordinates (px); test
  /// hooks.
  [[nodiscard]] double handlePosPx() const {
    return handle_pos_;
  }
  [[nodiscard]] double handleLenPx() const {
    return handle_len_;
  }

  /// Enables or disables pill-drag and scroll-on-drag (default true). False: the event filter still observes hover for
  /// show and hide but starts and continues no drag and consumes no mouse event; a drag active at that moment is
  /// cancelled. Timeline::setInteractionLocked uses it: the Scrollbar filter runs first in the LIFO chain, so it must
  /// be disabled explicitly, and returning early in mousePressEvent is not enough.
  void setInteractive(bool interactive);
  [[nodiscard]] bool isInteractive() const {
    return interactive_;
  }

  /// Enables click-to-scroll on the empty track (default false). False: only the visible handle is a drag target, so a
  /// press on the empty edge strip falls through to the underlying view and never steals a content click, the safe
  /// default for a pill over arbitrary content (adaptScrollAreas over plugin item views). True: a press anywhere in the
  /// strip grabs, first centering the handle under an off-handle press; only for a host that owns the whole viewport
  /// (the Timeline).
  void setClickToScroll(bool enabled);
  [[nodiscard]] bool clickToScroll() const {
    return click_to_scroll_;
  }

 signals:
  /// A user pill drag ended after changing the backing scrollbar.
  void scrollChangeCommitted();

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  /// Re-reads the accent color from the application palette on palette and style changes, unless setAccentColor()
  /// overrode it.
  void changeEvent(QEvent* event) override;

 private:
  /// Recomputes this widget's geometry (the viewport edge strip in overlay mode, the native bar's rect in
  /// reserved-gutter mode, both mapped into the area's coordinates) and the handle position and length from the live
  /// native bar; calls update().
  void recomputeGeometry();

  /// The whole eventFilter() body for reserved-gutter mode: blanks the native bar's painting, drives hover and scroll
  /// reveal, and tracks bar geometry. Never consumes a mouse event: the native bar owns all interaction.
  bool gutterEventFilter(QObject* watched, QEvent* event);

  /// Reveal-on-scroll shared by both placements: reveals this pill iff `event` carries a delta on its axis (Shift plus
  /// a vertical wheel counts as horizontal). Never consumes.
  void revealOnWheel(QWheelEvent* event);

  /// The area's native scroll bar matching orientation_; undefined behavior if area_ is null.
  QScrollBar* bar() const;

  /// Animates the pill to shown (opacity 1) or hidden (opacity 0). Ignored when auto_hide_ is false, and a show is
  /// suppressed when the bar has nothing to scroll (maximum <= minimum), as ScrollBarAsNeeded does.
  void setShown(bool shown);

  /// Reveals the pill and (re)arms the auto-hide hold timer, for a scroll (wheel or gesture) and as a grace period
  /// after a drag release that lands off the strip, so the pill lingers instead of vanishing. No-op when auto-hide is
  /// off (already pinned visible) or there is nothing to scroll. When the hold elapses, hide_timer_ hands control back
  /// to the hover path (stay shown iff the cursor is over the strip).
  void revealTemporarily();

  /// True while a scroll or interaction reveal still holds the pill visible (hide_timer_ armed). The hover path must
  /// not hide it then: a hover-out, or the synthetic MouseMove Qt delivers as scrolled content slides under a
  /// stationary cursor, would cancel the reveal too early. The hold timer owns the eventual hide.
  [[nodiscard]] bool scrollRevealActive() const;

  /// True when viewport_pos (viewport coordinates) is inside the edge strip of this orientation (the kHoverStripPx-wide
  /// band where the pill lives).
  bool pointInStrip(const QPoint& viewport_pos) const;

  /// Drives hover show and hide from a position already mapped into viewport coordinates: reveals inside the strip,
  /// hides outside (unless a scroll or interaction reveal still holds the pill). Shared by the viewport filter and the
  /// covering-child observers.
  void applyHoverAt(const QPoint& viewport_pos);

  /// Recursively enables mouse tracking on `widget` and installs this overlay as its event filter, so hover MouseMoves
  /// reach applyHoverAt() even when child widgets fully cover the viewport (a QScrollArea packed with cards); without
  /// it the pill would reveal only on scroll or click. New children are picked up through QEvent::ChildAdded in
  /// eventFilter(). Observe-only: the events are never consumed.
  void installHoverObserver(QWidget* widget);

  Qt::Orientation orientation_;
  Placement placement_ = Placement::kOverlayViewport;
  QAbstractScrollArea* area_ = nullptr;
  // The attached area as a plain QObject*, recorded while it was fully alive. eventFilter() checks it with qobject_cast
  // before using area_: during the area's destruction Qt still delivers events from its children, but the object is
  // already only a QWidget, so any QAbstractScrollArea member call on area_ is undefined behavior. qobject_cast
  // resolves through the virtual metaObject(), which reports the class of the destructor stage running, so it fails
  // safely.
  QObject* area_object_ = nullptr;
  // The cached viewport, a QPointer that nulls itself if the viewport is destroyed before this overlay. It guards
  // recomputeGeometry() (wired to the live bar's valueChanged and rangeChanged) against a dangling dereference during
  // host teardown, where the area's destructor can still emit a range or value change after the viewport is gone.
  QPointer<QWidget> viewport_;
  QColor accent_;
  double handle_pos_ = 0.0;
  double handle_len_ = 0.0;

  QGraphicsOpacityEffect* opacity_effect_ = nullptr;
  QPropertyAnimation* fade_anim_ = nullptr;
  // Single-shot timer that fades the pill out kScrollRevealHoldMs after the last scroll or interaction reveal
  // (revealTemporarily()); restarted on each.
  QTimer* hide_timer_ = nullptr;
  bool shown_ = false;
  bool auto_hide_ = true;
  int fade_ms_ = 150;

  // Drag-to-scroll state: set on MouseButtonPress in the strip, cleared on MouseButtonRelease. drag_start_value_ is
  // bar()->value() when the drag was captured (after any click-to-center jump); drag_start_axis_px_ is the viewport
  // coordinate along the scroll axis at the capture point.
  bool dragging_ = false;
  // True: a press anywhere in the strip grabs (off-handle presses jump the handle under the cursor first); false: only
  // the handle is a drag target. Opt-in (Timeline), so an overlay on foreign content never steals strip clicks.
  bool click_to_scroll_ = false;
  long drag_origin_value_ = 0;
  long drag_start_value_ = 0;
  double drag_start_axis_px_ = 0.0;

  // Interaction lock: when false the filter observes hover for show and hide but never starts or continues a drag and
  // never consumes mouse events. Set by setInteractive(); a drag is cleared at once on disable.
  bool interactive_ = true;
  // Reserved-gutter mode: a left press is held on the native bar (a native handle drag or track page). Keeps the pill
  // revealed while the drag's cursor wanders off the gutter, as dragging_ does for overlay.
  bool native_bar_pressed_ = false;

  // Reserved-gutter mode, item views only. The native gutter spans the area's full edge, including the band beside the
  // view's header, where a blank strip would read as a hole in the header. In two parts: recomputeGeometry() sets a
  // per-bar QSS margin that insets the native groove (and so the mirrored pill and native hit-testing) past the header;
  // and while standing in for the bar's eaten Paint, gutterEventFilter() paints that band as the header's continuation
  // (FrameworkTokens Backdrop and Separation: the QSS header background is not reachable for a foreign rect).
  QPointer<QHeaderView> gutter_header_;
  // The last header extent (px) applied as the bar's QSS margin; -1 = never set. Limits setStyleSheet (which
  // repolishes) to actual changes.
  int applied_header_inset_ = -1;
  // Whether accent_ was set with setAccentColor(). When false the color is re-read from the application palette on each
  // changeEvent, so the pill follows theme changes.
  bool accent_overridden_ = false;
};

/// Attaches canonical pill scrollbars (PJ::Scrollbar, reserved-gutter placement) to every QAbstractScrollArea under
/// `root`: the one call that gives an app window or dialog the scroll pills plugin dialogs use. The native bar keeps
/// reserving its layout gutter (the pill never covers content) but is painted blank; the pill overlay is its fading
/// visual. Idempotent: each adapted area is tagged with a "pjScrollbarAttached" dynamic property, so re-calling after
/// new views appear pills only the newcomers.
///
/// Per area:
///   - a combo-box's internal view and any transient popup item view are skipped;
///   - only a ScrollBarAsNeeded axis is adapted. AlwaysOn is a deliberate persistent native bar (kept, thin-styled by
///     the app QSS); AlwaysOff is a deliberately suppressed axis (the reflow-based "never scroll this axis" pattern)
///     where a reserved gutter would never appear: both are left untouched;
///   - the optional "pjScrollbarAutoHide" (bool) and "pjScrollbarFadeMs" (int) dynamic properties override the
///     overlay defaults.
///
/// `skip`, when set, is an extra per-area veto (true leaves an area's native bars untouched); the dialog host vetoes
/// its composite widgets' internal scroll areas with it.
void attachPillScrollbars(QWidget* root, const std::function<bool(QAbstractScrollArea*)>& skip = {});

}  // namespace PJ
