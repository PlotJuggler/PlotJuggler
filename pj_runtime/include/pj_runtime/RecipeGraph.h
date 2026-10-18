// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/types.hpp"
#include "pj_runtime/HistoryScope.h"
#include "pj_scripting/object_script.h"

namespace PJ {

/// The on-demand recipes and the DAG they form (docs/derived_objects/ARCHITECTURE.md §8, §13):
/// validation, binding every input in one dataset (D7), cycle rejection, ownership of previews,
/// pinned findings, and the life-cycle protocol verbs a restore calls. Pure logic: no Qt, no store,
/// no script runs, no threads. Results, scheduling and topics belong to its consumers.
///
/// Edges are named, never cached: a reader names its upstream by key, so removing an upstream
/// leaves the reader installed with a missing input, and reinstalling the key relinks it.
class RecipeGraph {
 public:
  struct Recipe {
    std::string owner_plugin;  ///< the plugin id: the key's namespace and the pin cap's unit
    std::string user_id;       ///< unique within the owner
    std::string language = "luau";
    std::string script;
    std::string params_json = "{}";
    /// Topic names, or `<key>/<output>` of another node's object output; also the aliases the
    /// script reads, a dataset qualifier included (`inputs['s:cloud']`).
    std::vector<std::string> inputs;
    std::vector<scripting::OutputSpec> outputs;
    /// A finding: evaluated at this time only, never following the cursor; also its label (D26).
    std::optional<Timestamp> pinned_t;
    /// A preview: owned by the plugin instance that created it, never persisted nor cleared by a restore.
    bool ephemeral = false;
    bool history_exempt = false;  ///< undo and redo have no authority over it
    /// The script's clock (`pj.tf.now`) is the requested instant; false: the newest resolved input stamp.
    bool query_time = true;

    [[nodiscard]] std::string key() const {
      return owner_plugin + "/" + user_id;
    }
  };

  /// One dataset an input resolves in. A dataset-qualified input (`s:cloud`) resolves only in its
  /// dataset; the resolver strips the qualifier, the recipe keeps it as the alias.
  struct InputCandidate {
    DatasetId dataset = 0;
    sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;  ///< kNone: a scalar series or field
    std::string topic;  ///< the name inside `dataset`, the qualifier stripped
  };
  /// Every dataset `input` resolves in, at most one candidate per dataset; empty when none. It never
  /// resolves an on-demand output, qualified or not: the graph binds those itself by their bare
  /// `<key>/<output>` name, so a qualified upstream name resolves nowhere and is refused as unknown.
  using InputResolver = std::function<std::vector<InputCandidate>(std::string_view input)>;

  /// One input of a node, parallel to `Recipe::inputs`.
  struct Binding {
    sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;  ///< kNone: a scalar series or field
    /// Key of the node whose object output this input reads (the input is `<upstream>/<output>`);
    /// empty for a topic of the dataset.
    std::string upstream;
    /// What the input reads: the bare topic inside the node's dataset, or the upstream's output name.
    std::string name;

    bool operator==(const Binding&) const = default;
  };

  struct Node {
    Recipe recipe;
    /// Graph-wide and monotonic, bumped when its meaning changes (an identical upsert keeps it); a key
    /// removed and reinstalled never reuses one, so a result of the removed node is never current.
    uint64_t revision = 0;
    DatasetId dataset = 0;  ///< where every input resolved, and where the outputs live
    std::vector<Binding> bindings;
    /// Compiled once per revision; the scheduler's runs share it.
    std::shared_ptr<const scripting::CompiledScript> compiled;
  };

  static constexpr std::size_t kMaxPinnedPerOwner = 64;

  explicit RecipeGraph(InputResolver resolver);

  /// Installs `recipe`, replacing the node of the same key. Rejected, with nothing changed: an empty
  /// owner or id; a language other than Luau; a script that does not compile; no input or no output;
  /// an output `scripting::parseOutputSpec` refuses (only types a script can produce); a name used
  /// twice among the inputs and outputs; an input that resolves nowhere, or whose datasets have no
  /// single one in common (D7); an object input whose type has no field table; its own output; an
  /// edge that closes a cycle; a persistent recipe reading a preview's output, or a preview that a
  /// persistent node reads (a preview ends with its panel, the persisted reader would not); a new pin
  /// past the owner's cap.
  [[nodiscard]] Status upsert(Recipe recipe);
  /// Puts back `node`, a copy of the key's node taken before an `upsert` replaced it, revision and all: how a
  /// consumer that cannot follow that upsert undoes it. The revision the upsert took is never reused.
  void reinstate(Node node);
  /// Forgets the node. Its readers stay installed and report the input missing (`missingInput`).
  [[nodiscard]] Status remove(std::string_view key);
  /// `remove` for a pinned finding; rejects an unknown or unpinned key.
  [[nodiscard]] Status dismiss(std::string_view key);
  /// Removes every finding of `owner_plugin`; returns whether any was removed.
  bool dismissAll(std::string_view owner_plugin);
  /// Removes the previews among `keys`: what one plugin instance created, when it ends. A key that
  /// is gone or persistent is skipped. Returns whether anything was removed.
  bool removeEphemerals(const std::unordered_set<std::string>& keys);

  [[nodiscard]] const Node* find(std::string_view key) const;
  /// Why `key` cannot run whatever the time: an upstream, direct or transitive, was removed or no
  /// longer provides the output it reads (renamed, retyped, or moved to another dataset). nullopt
  /// when every upstream provides, and for an unknown key.
  [[nodiscard]] std::optional<std::string> missingInput(std::string_view key) const;
  /// Keys of every node reading an output of one of `keys`, transitively; sorted, `keys` excluded.
  [[nodiscard]] std::vector<std::string> dependentsOf(const std::vector<std::string>& keys) const;

  // The life-cycle protocol (ARCHITECTURE §13). A restore has authority over every persistent node
  // under `kReplace`, and over the non-exempt ones under `kHistory`; never over a preview.

  /// Keys of the nodes in the restore's authority that `wanted` describes unchanged: every recipe
  /// field equal (script and params ignoring line endings), and no upstream the restore re-creates
  /// or removes (one changed, removed, or absent but wanted). The revision is not compared.
  [[nodiscard]] std::unordered_set<std::string> unchangedRecipeKeys(
      const std::vector<Recipe>& wanted, RestoreIntent intent) const;
  /// Removes every node in the restore's authority except `keep`.
  void clearRecipes(RestoreIntent intent, const std::unordered_set<std::string>& keep = {});
  [[nodiscard]] bool hasHistoryExemptRecipes() const;
  /// Output names (`<key>/<output>`) of the persistent exempt nodes reading a topic in `removed`
  /// (the topic or a slash-bounded field below it), directly or through upstream nodes: what a
  /// history restore removing those topics would strand.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<std::string>& removed) const;

 private:
  /// The key of the live node with an object output named `input` (`<key>/<output>`), or null.
  [[nodiscard]] const std::string* producerOf(std::string_view input) const;

  InputResolver resolver_;
  std::map<std::string, Node, std::less<>> nodes_;
  uint64_t next_revision_ = 1;
};

}  // namespace PJ
