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
#include "pj_scripting/on_demand_script.h"

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
  /// One input of a node, parallel to `Recipe::inputs`.
  struct Binding {
    sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;  ///< kNone: a scalar series or field
    /// Key of the node whose object output this input reads (`<upstream>/<output>`, maybe qualified);
    /// empty for a topic of the dataset.
    std::string upstream;
    /// What the input reads: the bare topic inside the node's dataset, or the upstream's output name.
    std::string name;

    bool operator==(const Binding&) const = default;
  };

  struct Recipe {
    std::string owner_plugin;  ///< the plugin id: the key's namespace and the pin cap's unit
    std::string user_id;       ///< unique within the owner
    std::string language = "luau";
    std::string script;
    std::string params_json = "{}";
    /// Topic names, or `<key>/<output>` of another node's object output, either one maybe behind its
    /// dataset's qualifier; also the aliases the script reads, the qualifier included (`inputs['s:cloud']`).
    std::vector<std::string> inputs;
    std::vector<scripting::OutputSpec> outputs;
    /// The inputs a layout recorded as other nodes' object outputs, by input name. One no installed node provides
    /// (its node is absent, or no longer has that output) binds to the recorded node anyway, missing its input (the
    /// state removing or renaming leaves a reader in, `missingInput`), instead of being refused as unknown; a node
    /// that provides it binds the others. Not part of the recipe's meaning.
    std::map<std::string, Binding, std::less<>> recorded_upstreams;
    /// The topic a layout recorded a qualified input reading, by input name: tried when the input resolves nowhere as
    /// written or not on `dataset`, because its qualifier names another recording (a generic layout moved to a new
    /// one). The input stays the alias the script reads. Not part of the recipe's meaning.
    std::map<std::string, std::string, std::less<>> recorded_topics;
    /// The dataset every input must resolve in (a layout's saved input dataset, a request's dataset id); nullopt:
    /// the one dataset carrying all of them (D7).
    std::optional<DatasetId> dataset;
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
  /// resolves an on-demand output, qualified or not: the graph binds those itself (`QualifierSplitter`).
  using InputResolver = std::function<std::vector<InputCandidate>(std::string_view input)>;

  /// The dataset qualifier an input starts with.
  struct Qualifier {
    std::string bare;                 ///< the input without its qualifier
    std::vector<DatasetId> datasets;  ///< every dataset carrying the qualifier's source
  };
  /// The loaded source name `input` starts with followed by ':' (longest match wins, never a blind split at ':');
  /// nullopt when none does. How the graph reads `<source>:<key>/<output>`, the form a dataset-qualified editor
  /// writes, as another node's output: it binds when the qualifier names the producer's dataset, and is refused
  /// otherwise, unless the recipe recorded that producer; naming the recipe's own output so is refused as the bare form
  /// is. Empty: only the bare form names an output.
  using QualifierSplitter = std::function<std::optional<Qualifier>(std::string_view input)>;

  struct Node {
    Recipe recipe;
    /// Graph-wide and monotonic, bumped when its meaning changes (an identical upsert keeps it); a key
    /// removed and reinstalled never reuses one, so a result of the removed node is never current.
    uint64_t revision = 0;
    DatasetId dataset = 0;  ///< where every input resolved, and where the outputs live
    std::vector<Binding> bindings;
    /// Compiled once per revision in the recipe's language (Python by its first run), which also picks its runs'
    /// lane; the runs share it.
    std::shared_ptr<const scripting::CompiledOnDemandScript> compiled;
  };

  static constexpr std::size_t kMaxPinnedPerOwner = 64;
  /// What a plugin's preview id starts with. No persistent recipe's id starts with `~`, so a preview never holds a
  /// key a layout can name (a restore never clears a preview, and refuses one holding a wanted key).
  static constexpr std::string_view kPreviewIdPrefix = "~preview/";

  explicit RecipeGraph(InputResolver resolver, QualifierSplitter qualifier = {});

  /// Installs `recipe`, replacing the node of the same key; empty params are stored as "{}", what a
  /// layout parse reads back. Rejected, with nothing changed: an empty owner or id; a key another owner's
  /// recipe holds (an id may contain '/', so two owners can spell one key); a language no runner has (Luau, and Python
  /// where `scripting::pythonAvailable()`);
  /// params nested deeper than `scripting::kMaxParamsDepth`; a Luau script that does not compile (a Python one
  /// compiles at its first run, which reports its error, so an upsert never waits for the GIL); no input
  /// or no output; an output `scripting::parseOutputSpec` refuses (only types a script can produce); a
  /// name used twice among the inputs and outputs; a persistent id starting with `~` (`kPreviewIdPrefix`);
  /// an input that resolves nowhere (unless recorded as a topic, or as an absent node's output), another node's output
  /// qualified with a dataset that node is not on and not recorded as read, an input that its `Recipe::dataset` does
  /// not carry, or whose datasets have no single one in common (D7); an object input whose type has no field table; its
  /// own output, bare or qualified; an edge that closes a cycle; a persistent recipe reading a preview's output, or a
  /// preview that a persistent node reads (a preview ends with its panel, the persisted reader would not);
  /// `history_exempt` changing on an installed key (history would hold its snapshots under the other authority: remove
  /// it and create it again); a new pin past the owner's cap.
  [[nodiscard]] Status upsert(Recipe recipe);
  /// Validates and binds `recipe` as `upsert` does, installing nothing: the node of a transient evaluation, at
  /// revision 0 (`GridRunner`). It may declare no output (the evaluation infers them); a pin, a preview and
  /// `history_exempt` change
  /// nothing here, since nothing persists. A transient evaluation is a preview by definition: mark it `ephemeral`, or
  /// it may not read a preview's output.
  [[nodiscard]] Expected<Node> bind(Recipe recipe) const;
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
  /// Keys of the nodes with a topic input rooted at a name in `topics` (the topic or a slash-bounded
  /// field below it), sorted: what reads a processor output a restore re-creates.
  [[nodiscard]] std::vector<std::string> readersOfSeries(const std::vector<std::string>& topics) const;
  /// Keys of the nodes bound to `dataset`, sorted.
  [[nodiscard]] std::vector<std::string> keysOf(DatasetId dataset) const;
  /// Every key, sorted.
  [[nodiscard]] std::vector<std::string> keys() const;
  [[nodiscard]] bool empty() const {
    return nodes_.empty();
  }

  // The life-cycle protocol (ARCHITECTURE §13). A restore has authority over every persistent node
  // under `kReplace`, and over the non-exempt ones under `kHistory`; never over a preview.

  /// Keys of the nodes in the restore's authority that `wanted` describes unchanged: every recipe
  /// field equal (script and params ignoring line endings; a wanted `Recipe::dataset` is the node's),
  /// no upstream the restore re-creates or removes (one changed, removed, or absent but wanted), and no
  /// topic input rooted at a name in `replaced_outputs` (the transform and filter outputs the restore
  /// removes or creates; a series key at the name or a slash-bounded field below it). Transitive. The
  /// revision is not compared.
  [[nodiscard]] std::unordered_set<std::string> unchangedRecipeKeys(
      const std::vector<Recipe>& wanted, RestoreIntent intent,
      const std::vector<std::string>& replaced_outputs = {}) const;
  /// Removes every node in the restore's authority except `keep`; returns the removed keys, sorted.
  std::vector<std::string> clearRecipes(RestoreIntent intent, const std::unordered_set<std::string>& keep = {});
  [[nodiscard]] bool hasHistoryExemptRecipes() const;
  /// Output names (`<key>/<output>`) of the persistent exempt nodes reading a topic in `removed`
  /// (the topic or a slash-bounded field below it), directly or through upstream nodes: what a
  /// history restore removing those topics would strand.
  [[nodiscard]] std::vector<std::string> exemptDependentsOf(const std::vector<std::string>& removed) const;

 private:
  /// How an input names another node's output, bare or behind a qualifier (`QualifierSplitter`).
  struct UpstreamOutput {
    const std::string* producer = nullptr;                     ///< the live node owning the output, or null
    std::string output_name;                                   ///< `<key>/<output>`: the input, a qualifier stripped
    std::optional<std::vector<DatasetId>> qualifier_datasets;  ///< the datasets a qualifier names; nullopt: none
  };
  [[nodiscard]] UpstreamOutput upstreamOutput(std::string_view input) const;
  /// The key of the live node with an object output named `input` (`<key>/<output>`), or null.
  [[nodiscard]] const std::string* producerOf(std::string_view input) const;

  InputResolver resolver_;
  QualifierSplitter qualifier_;
  std::map<std::string, Node, std::less<>> nodes_;
  uint64_t next_revision_ = 1;
};

}  // namespace PJ
