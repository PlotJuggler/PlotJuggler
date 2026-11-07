#pragma once

namespace PJ {

// Contract for widgets that render object-store topics as layers (2D media today,
// 3D later), implemented in addition to IDataWidget. Lets the shell keep viewers
// coherent with object removal without knowing how many topics a viewer composes.
class IObjectViewer {
 public:
  virtual ~IObjectViewer() = default;

  // Pull-based re-validation against the live ObjectStore: drop layers whose topic
  // was evicted, keep the rest. Returns true if the dock should be kept — either a
  // live layer remains, OR the dock is intentionally empty (never populated: a
  // click-created or restored-empty scene, or emptied by a caller: detachTopic).
  // Returns false only when a dock that *had* content is now empty because it was
  // all evicted, in which case the shell resets it to the placeholder (a plugin's
  // own tab excepted).
  virtual bool revalidateObjects() = 0;
};

}  // namespace PJ
