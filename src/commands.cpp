// Cobra/CLI11 wiring: global flags, JSON output parity with Go mnemon, command handlers.
#include "commands.hpp"

#include "db.hpp"
#include "diff.hpp"
#include "graph_bfs.hpp"
#include "graph_causal.hpp"
#include "graph_engine.hpp"
#include "graph_extract.hpp"
#include "graph_semantic.hpp"
#include "intent.hpp"
#include "keyword.hpp"
#include "model_json.hpp"
#include "ollama.hpp"
#include "paths.hpp"
#include "recall.hpp"
#include "setup.hpp"
#include "time_util.hpp"
#include "uuid.hpp"
#include "vector_math.hpp"
#include "viz.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace fs = std::filesystem;

static std::string g_data_dir;
static std::string g_store_flag;
static bool g_readonly = false;

#ifndef MNEMON_VERSION_STR
#define MNEMON_VERSION_STR "dev"
#endif
static const char* kVersion = MNEMON_VERSION_STR;

static void print_json(const nlohmann::json& j) { std::cout << j.dump(2) << '\n'; }

static std::string resolve_store() {
  if (!g_store_flag.empty()) {
    return g_store_flag;
  }
  if (const char* e = std::getenv("MNEMON_STORE"); e && *e) {
    return e;
  }
  return mnemon::paths::read_active(g_data_dir);
}

static std::unique_ptr<mnemon::Database> open_db() {
  std::string name = resolve_store();
  if (!mnemon::paths::valid_store_name(name)) {
    throw std::runtime_error("invalid store name \"" + name + "\"");
  }
  std::string dir = mnemon::paths::store_dir(g_data_dir, name);
  if (g_readonly) {
    return mnemon::Database::open_readonly(dir);
  }
  if (!mnemon::paths::migrate_if_needed(g_data_dir, false)) {
    throw std::runtime_error("migrate failed");
  }
  return mnemon::Database::open_readwrite(dir);
}

static void require_positive_limit(const char* flag, int value) {
  if (value < 1) {
    throw std::runtime_error(std::string(flag) + " must be at least 1, got " + std::to_string(value));
  }
}

static void require_non_negative_float(const char* flag, double value) {
  if (value < 0.0) {
    std::ostringstream oss;
    oss << flag << " must be non-negative, got " << value;
    throw std::runtime_error(oss.str());
  }
}

static bool valid_category(const std::string& c) {
  static const char* ok[] = {"preference", "decision", "fact", "insight", "context", "general"};
  for (auto* x : ok) {
    if (c == x) {
      return true;
    }
  }
  return false;
}

static std::vector<std::string> split_comma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == ',') {
      while (!cur.empty() && cur.front() == ' ') {
        cur.erase(cur.begin());
      }
      while (!cur.empty() && cur.back() == ' ') {
        cur.pop_back();
      }
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  while (!cur.empty() && cur.front() == ' ') {
    cur.erase(cur.begin());
  }
  while (!cur.empty() && cur.back() == ' ') {
    cur.pop_back();
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

static nlohmann::json recall_result_json(const mnemon::search_engine::RecallResult& r) {
  nlohmann::json j;
  j["insight"] = mnemon::insight_to_json(r.insight);
  j["score"] = r.score;
  j["intent"] = mnemon::search_engine::intent_str(r.intent);
  if (!r.via.empty()) {
    j["via"] = r.via;
  }
  j["signals"] = {{"keyword", r.signals.keyword},
                  {"entity", r.signals.entity},
                  {"similarity", r.signals.similarity},
                  {"graph", r.signals.graph}};
  return j;
}

int run_mnemon(int argc, char** argv) {
  CLI::App app{"Mnemon is a standalone memory daemon based on MAGMA's four-graph architecture."};
  app.set_help_flag("-h,--help", "Print this help message and exit");
  app.add_flag_function("-v,--version", [](int) {
    std::cout << kVersion << "\n";
    std::exit(0);
  });

  g_data_dir = mnemon::paths::default_data_dir();
  app.add_option("--data-dir", g_data_dir, "base data directory (env: MNEMON_DATA_DIR)");
  app.add_option("--store", g_store_flag, "named memory store (overrides MNEMON_STORE and active file)");
  app.add_flag("--readonly", g_readonly, "open database in read-only mode");

  auto trunc8 = [](const std::string& id) { return id.size() > 8 ? id.substr(0, 8) : id; };

  // --- remember ---
  auto* remember = app.add_subcommand("remember", "Store a new insight");
  std::vector<std::string> rem_parts;
  std::string rem_cat = "general";
  int rem_imp = 3;
  std::string rem_tags;
  std::string rem_source = "user";
  std::string rem_entities;
  std::string rem_entity_mode = "merge";
  bool rem_no_diff = false;
  remember->add_option("content", rem_parts, "insight text")->required()->expected(-1);
  remember->add_option("--cat", rem_cat, "category");
  remember->add_option("--imp", rem_imp, "importance 1-5");
  remember->add_option("--tags", rem_tags, "comma-separated tags");
  remember->add_option("--source", rem_source, "source");
  remember->add_option("--entities", rem_entities, "comma-separated entities");
  remember->add_option("--entity-mode", rem_entity_mode, "entity extraction mode: merge, provided, auto");
  remember->add_flag("--no-diff", rem_no_diff);
  remember->callback([&] {
    std::string rem_content;
    for (size_t i = 0; i < rem_parts.size(); ++i) {
      if (i) {
        rem_content += ' ';
      }
      rem_content += rem_parts[i];
    }
    if (rem_content.size() > 8000) {
      throw CLI::ValidationError(
          "content too long (" + std::to_string(rem_content.size()) +
          " chars, max 8000); consider chunking into multiple remember calls");
    }
    if (!valid_category(rem_cat)) {
      throw CLI::ValidationError("invalid category \"" + rem_cat +
                               "\"; valid: preference, decision, fact, insight, context, general");
    }
    if (rem_imp < 1 || rem_imp > 5) {
      throw CLI::ValidationError("importance must be 1-5, got " + std::to_string(rem_imp));
    }
    auto tags = split_comma(rem_tags);
    if (tags.size() > 20) {
      throw CLI::ValidationError("too many tags (" + std::to_string(tags.size()) + ", max 20)");
    }
    for (const auto& t : tags) {
      if (t.size() > 100) {
        throw CLI::ValidationError("tag too long (" + std::to_string(t.size()) + " chars, max 100): " + t.substr(0, 50));
      }
    }
    auto ent_in = split_comma(rem_entities);
    if (ent_in.size() > 50) {
      throw CLI::ValidationError("too many entities (" + std::to_string(ent_in.size()) + ", max 50)");
    }
    for (const auto& e : ent_in) {
      if (e.size() > 200) {
        throw CLI::ValidationError("entity too long (" + std::to_string(e.size()) + " chars, max 200): " + e.substr(0, 50));
      }
    }
    if (!mnemon::graph_eng::valid_entity_mode(rem_entity_mode)) {
      throw std::runtime_error("invalid entity mode \"" + rem_entity_mode + "\"; valid: merge, provided, auto");
    }

    auto db = open_db();
    mnemon::OllamaClient oc = mnemon::OllamaClient::from_env();
    std::vector<double> embed_vec;
    std::vector<uint8_t> embed_blob;
    if (oc.available()) {
      try {
        embed_vec = oc.embed(rem_content);
        embed_blob = mnemon::serialize_vector(embed_vec);
      } catch (...) {
        embed_vec.clear();
        embed_blob.clear();
      }
    }

    mnemon::graph_eng::EmbedCache embed_cache;
    if (oc.available()) {
      embed_cache = mnemon::graph_eng::build_embed_cache(*db);
    }

    mnemon::search_engine::DiffSuggestion diff_sug = mnemon::search_engine::DiffSuggestion::Add;
    std::string diff_action = "added";
    std::string replaced_id;
    if (!rem_no_diff) {
      std::vector<mnemon::search_engine::EmbeddedItem> eitems;
      for (const auto& [id, vec] : embed_cache) {
        eitems.push_back({id, vec});
      }
      mnemon::search_engine::DiffOptions dopts;
      dopts.limit = 5;
      dopts.new_embedding = embed_vec;
      dopts.existing_embed = std::move(eitems);
      auto dres = mnemon::search_engine::diff_insights(db->get_all_active_insights(), rem_content, dopts);
      diff_sug = dres.suggestion;
      if (dres.suggestion == mnemon::search_engine::DiffSuggestion::Duplicate) {
        diff_action = "skipped";
        if (!dres.matches.empty()) {
          replaced_id = dres.matches[0].id;
        }
      } else if (dres.suggestion == mnemon::search_engine::DiffSuggestion::Conflict ||
                 dres.suggestion == mnemon::search_engine::DiffSuggestion::Update) {
        diff_action = "updated";
        if (!dres.matches.empty()) {
          replaced_id = dres.matches[0].id;
        }
      }
    }

    mnemon::Insight insight;
    insight.id = mnemon::new_uuid_v4();
    insight.content = rem_content;
    insight.category = rem_cat;
    insight.importance = rem_imp;
    insight.tags = tags;
    insight.entities = ent_in;
    insight.source = rem_source;
    auto now = mnemon::time_util::now_utc();
    insight.created_at = now;
    insight.updated_at = now;

    if (diff_action == "skipped") {
      db->log_op("diff-skip", insight.id, "duplicate of " + replaced_id);
      nlohmann::json o;
      o["id"] = insight.id;
      o["content"] = rem_content;
      o["action"] = "skipped";
      o["diff_suggestion"] = mnemon::search_engine::diff_suggestion_str(diff_sug);
      o["replaced_id"] = replaced_id;
      print_json(o);
      return;
    }

    mnemon::graph_eng::EmbedCache* ec_ptr = oc.available() ? &embed_cache : nullptr;
    if (oc.available() && !embed_vec.empty()) {
      ec_ptr = &embed_cache;
    }

    mnemon::graph_eng::EdgeStats estats;
    double ei = 0;
    int pruned = 0;
    bool embedded = false;

    try {
      db->in_transaction([&] {
        if (diff_action == "updated" && !replaced_id.empty()) {
          try {
            db->soft_delete_insight(replaced_id);
            db->log_op("diff-replace", replaced_id, "replaced by " + insight.id);
            embed_cache.erase(replaced_id);
          } catch (const std::exception& ex) {
            std::cerr << "warning: soft-delete " << replaced_id << ": " << ex.what() << "\n";
          }
        }
        db->insert_insight(insight);
        if (!embed_blob.empty()) {
          db->update_embedding(insight.id, embed_blob);
          embedded = true;
          embed_cache[insight.id] = embed_vec;
        }
        estats = mnemon::graph_eng::on_insight_created(*db, insight, ec_ptr, rem_entity_mode);
        if (!insight.entities.empty()) {
          db->update_entities(insight.id, insight.entities);
        }
        auto p = db->refresh_effective_importance(insight.id);
        ei = p.first;
        pruned = db->auto_prune(mnemon::kMaxInsights, {insight.id});
        db->log_op("remember", insight.id, insight.content);
      });
    } catch (...) {
      throw;
    }

    auto sem = mnemon::graph_eng::find_semantic_candidates(*db, insight, ec_ptr);
    auto cau = mnemon::graph_eng::find_causal_candidates(*db, insight);
    nlohmann::json o;
    o["id"] = insight.id;
    o["content"] = insight.content;
    o["category"] = insight.category;
    o["importance"] = insight.importance;
    o["tags"] = insight.tags;
    o["entities"] = insight.entities;
    o["action"] = diff_action;
    o["diff_suggestion"] = mnemon::search_engine::diff_suggestion_str(diff_sug);
    o["created_at"] = mnemon::time_util::rfc3339_utc(insight.created_at);
    o["edges_created"] = {{"temporal", estats.temporal},
                          {"entity", estats.entity},
                          {"causal", estats.causal},
                          {"semantic", estats.semantic}};
    nlohmann::json sj = nlohmann::json::array();
    for (const auto& s : sem) {
      sj.push_back({{"id", s.id},
                    {"content", s.content},
                    {"category", s.category},
                    {"similarity", s.similarity},
                    {"auto_linked", s.auto_linked}});
    }
    o["semantic_candidates"] = sj;
    nlohmann::json cj = nlohmann::json::array();
    for (const auto& c : cau) {
      cj.push_back({{"id", c.id},
                    {"content", c.content},
                    {"category", c.category},
                    {"hop", c.hop},
                    {"via_edge", c.via_edge},
                    {"causal_signal", c.causal_signal},
                    {"suggested_sub_type", c.suggested_sub_type}});
    }
    o["causal_candidates"] = cj;
    o["embedded"] = embedded;
    o["effective_importance"] = ei;
    o["auto_pruned"] = pruned;
    if (!replaced_id.empty()) {
      o["replaced_id"] = replaced_id;
    }
    print_json(o);
  });

  // --- recall ---
  auto* recall = app.add_subcommand("recall", "Retrieve insights");
  std::vector<std::string> rec_parts;
  std::string rec_cat;
  std::string rec_source;
  int rec_limit = 10;
  bool rec_basic = false;
  bool rec_smart = false;
  std::string rec_intent;
  recall->add_option("query", rec_parts)->required()->expected(-1);
  recall->add_option("--cat", rec_cat);
  recall->add_option("--source", rec_source);
  recall->add_option("--limit", rec_limit);
  recall->add_flag("--basic", rec_basic);
  recall->add_flag("--smart", rec_smart)->group("");
  recall->add_option("--intent", rec_intent);
  recall->callback([&] {
    require_positive_limit("--limit", rec_limit);
    std::string rec_query;
    for (size_t i = 0; i < rec_parts.size(); ++i) {
      if (i) {
        rec_query += ' ';
      }
      rec_query += rec_parts[i];
    }
    auto db = open_db();
    if (rec_basic) {
      mnemon::QueryFilter qf;
      qf.keyword = rec_query;
      qf.category = rec_cat;
      qf.source = rec_source;
      qf.limit = rec_limit;
      auto results = db->query_insights(qf);
      for (const auto& r : results) {
        db->increment_access_count(r.id);
      }
      db->log_op("recall:basic", "", "q=" + rec_query + " hits=" + std::to_string(results.size()));
      nlohmann::json arr = nlohmann::json::array();
      for (const auto& i : results) {
        arr.push_back(mnemon::insight_to_json(i));
      }
      print_json(arr);
      return;
    }
    std::optional<mnemon::search_engine::Intent> ov;
    if (!rec_intent.empty()) {
      auto p = mnemon::search_engine::intent_from_string(rec_intent);
      if (!p) {
        throw CLI::ValidationError("unknown intent \"" + rec_intent + "\"; valid: WHY, WHEN, ENTITY, GENERAL");
      }
      ov = *p;
    }
    std::vector<double> qvec;
    mnemon::OllamaClient oc = mnemon::OllamaClient::from_env();
    if (oc.available()) {
      try {
        qvec = oc.embed(rec_query);
      } catch (...) {
        qvec.clear();
      }
    }
    auto qents = mnemon::graph_eng::extract_entities(rec_query);
    auto resp =
        mnemon::search_engine::intent_aware_recall(*db, rec_query, qvec, qents, rec_limit, ov);
    for (const auto& r : resp.results) {
      db->increment_access_count(r.insight.id);
    }
    db->log_op("recall", "", "q=" + rec_query + " hits=" + std::to_string(resp.results.size()));
    nlohmann::json out;
    nlohmann::json rj = nlohmann::json::array();
    for (const auto& r : resp.results) {
      rj.push_back(recall_result_json(r));
    }
    out["results"] = rj;
    nlohmann::json meta;
    meta["intent"] = mnemon::search_engine::intent_str(resp.meta.intent);
    meta["intent_source"] = resp.meta.intent_source;
    meta["anchor_count"] = resp.meta.anchor_count;
    meta["traversed"] = resp.meta.traversed;
    if (!resp.meta.hint.empty()) {
      meta["hint"] = resp.meta.hint;
    }
    out["meta"] = meta;
    print_json(out);
  });

  // --- search ---
  auto* search = app.add_subcommand("search", "Token search");
  std::vector<std::string> sea_parts;
  int sea_limit = 10;
  search->add_option("query", sea_parts)->required()->expected(-1);
  search->add_option("--limit", sea_limit);
  search->callback([&] {
    require_positive_limit("--limit", sea_limit);
    std::string sea_q;
    for (size_t i = 0; i < sea_parts.size(); ++i) {
      if (i) {
        sea_q += ' ';
      }
      sea_q += sea_parts[i];
    }
    auto db = open_db();
    auto all = db->get_all_active_insights();
    auto scored = mnemon::search_engine::keyword_search(all, sea_q, sea_limit);
    for (const auto& s : scored) {
      db->increment_access_count(s.insight.id);
    }
    db->log_op("search", "", "q=" + sea_q + " hits=" + std::to_string(scored.size()));
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : scored) {
      arr.push_back({{"id", s.insight.id},
                     {"content", s.insight.content},
                     {"category", s.insight.category},
                     {"importance", s.insight.importance},
                     {"tags", s.insight.tags},
                     {"score", s.score}});
    }
    print_json(arr);
  });

  // --- link ---
  auto* link = app.add_subcommand("link", "Link insights");
  std::string l_src, l_tgt;
  std::string l_type = "semantic";
  double l_weight = 0.5;
  std::string l_meta;
  link->add_option("source", l_src)->required();
  link->add_option("target", l_tgt)->required();
  link->add_option("--type", l_type);
  link->add_option("--weight", l_weight);
  link->add_option("--meta", l_meta);
  link->callback([&] {
    auto et = mnemon::parse_edge_type(l_type);
    if (!et) {
      throw CLI::ValidationError("invalid edge type \"" + l_type + "\"; valid: temporal, semantic, causal, entity");
    }
    if (l_weight < 0 || l_weight > 1.0) {
      std::ostringstream os;
      os.setf(std::ios::fixed);
      os.precision(2);
      os << "weight must be between 0.0 and 1.0, got " << l_weight;
      throw CLI::ValidationError(os.str());
    }
    auto db = open_db();
    if (!db->get_insight_by_id(l_src)) {
      throw std::runtime_error("source insight " + l_src + " not found");
    }
    if (!db->get_insight_by_id(l_tgt)) {
      throw std::runtime_error("target insight " + l_tgt + " not found");
    }
    std::map<std::string, std::string> metadata = {{"created_by", "claude"}};
    if (!l_meta.empty()) {
      auto mj = nlohmann::json::parse(l_meta, nullptr, false);
      if (!mj.is_object()) {
        throw CLI::ValidationError("invalid metadata JSON");
      }
      for (auto it = mj.begin(); it != mj.end(); ++it) {
        if (!it.value().is_string()) {
          throw CLI::ValidationError("--meta values must be JSON strings (match Go map[string]string)");
        }
        metadata[it.key()] = it.value().get<std::string>();
      }
      metadata["created_by"] = "claude";
    }
    auto now = mnemon::time_util::now_utc();
    mnemon::Edge e1{l_src, l_tgt, *et, l_weight, metadata, now};
    mnemon::Edge e2{l_tgt, l_src, *et, l_weight, metadata, now};
    db->insert_edge(e1);
    db->insert_edge(e2);
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << trunc8(l_src) << "→" << trunc8(l_tgt) << " type=" << l_type << " weight=" << l_weight;
    db->log_op("link", l_src, os.str());
    nlohmann::json o;
    o["status"] = "linked";
    o["source_id"] = l_src;
    o["target_id"] = l_tgt;
    o["edge_type"] = l_type;
    o["weight"] = l_weight;
    o["metadata"] = metadata;
    print_json(o);
  });

  // --- related ---
  auto* related = app.add_subcommand("related", "Related insights");
  std::string rel_id;
  std::string rel_edge;
  int rel_depth = 2;
  related->add_option("id", rel_id)->required();
  related->add_option("--edge", rel_edge);
  related->add_option("--depth", rel_depth);
  related->callback([&] {
    auto db = open_db();
    if (!db->get_insight_by_id(rel_id)) {
      throw std::runtime_error("insight not found");
    }
    std::optional<mnemon::EdgeType> ef;
    if (!rel_edge.empty()) {
      auto p = mnemon::parse_edge_type(rel_edge);
      if (!p) {
        throw CLI::ValidationError("invalid edge type \"" + rel_edge + "\"; valid: temporal, semantic, causal, entity");
      }
      ef = *p;
    }
    auto nodes = mnemon::graph_eng::bfs(*db, rel_id, {rel_depth, 0, ef});
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : nodes) {
      nlohmann::json item;
      item["id"] = n.insight.id;
      item["content"] = n.insight.content;
      item["category"] = n.insight.category;
      item["importance"] = n.insight.importance;
      item["depth"] = n.hop;
      item["via_edge_type"] = mnemon::edge_type_str(n.via_edge.edge_type);
      arr.push_back(std::move(item));
    }
    print_json(arr);
  });

  // --- forget ---
  auto* forget = app.add_subcommand("forget", "Soft-delete");
  std::string forg_id;
  forget->add_option("id", forg_id)->required();
  forget->callback([&] {
    auto db = open_db();
    db->soft_delete_insight(forg_id);
    db->log_op("forget", forg_id, "");
    print_json(nlohmann::json{{"id", forg_id}, {"status", "deleted"}, {"message", "Insight soft-deleted successfully"}});
  });

  // --- gc ---
  auto* gc = app.add_subcommand("gc", "Retention GC");
  double gc_thr = 0.5;
  int gc_lim = 20;
  std::string gc_keep;
  gc->add_option("--threshold", gc_thr);
  gc->add_option("--limit", gc_lim);
  gc->add_option("--keep", gc_keep);
  gc->callback([&] {
    require_positive_limit("--limit", gc_lim);
    require_non_negative_float("--threshold", gc_thr);
    auto db = open_db();
    if (!gc_keep.empty()) {
      auto ins = db->get_insight_by_id(gc_keep);
      if (!ins) {
        throw std::runtime_error("insight " + gc_keep + " not found");
      }
      db->boost_retention(gc_keep);
      double ei = db->refresh_effective_importance(gc_keep).first;
      db->log_op("gc_keep", gc_keep, ins->content);
      int na = ins->access_count + 3;
      print_json(nlohmann::json{{"status", "retained"},
                                {"id", gc_keep},
                                {"content", ins->content},
                                {"new_access", na},
                                {"effective_importance", ei},
                                {"immune", db->is_immune(ins->importance, na)}});
      return;
    }
    auto [cands, total] = db->get_retention_candidates(gc_thr, gc_lim);
    std::ostringstream ld;
    ld << "threshold=" << std::fixed << std::setprecision(2) << gc_thr << " found=" << cands.size()
       << " total=" << total;
    db->log_op("gc", "", ld.str());
    nlohmann::json cj = nlohmann::json::array();
    for (const auto& c : cands) {
      cj.push_back({{"insight", mnemon::insight_to_json(c.insight)},
                    {"effective_importance", c.effective_importance},
                    {"days_since_access", c.days_since_access},
                    {"edge_count", c.edge_count},
                    {"immune", c.immune}});
    }
    print_json(nlohmann::json{{"total_insights", total},
                              {"threshold", gc_thr},
                              {"candidates_found", static_cast<int>(cands.size())},
                              {"candidates", cj},
                              {"max_insights", mnemon::kMaxInsights},
                              {"actions",
                               {{"purge", "mnemon forget <id>"}, {"keep", "mnemon gc --keep <id>"}}}});
  });

  // --- embed ---
  auto* embed = app.add_subcommand("embed", "Embeddings");
  std::string emb_id;
  bool emb_all = false;
  bool emb_status = false;
  embed->add_option("id", emb_id);
  embed->add_flag("--all", emb_all);
  embed->add_flag("--status", emb_status);
  embed->callback([&] {
    auto db = open_db();
    mnemon::OllamaClient oc = mnemon::OllamaClient::from_env();
    if (emb_status) {
      auto [tot, emb] = db->embedding_stats();
      int pct = tot > 0 ? static_cast<int>(std::lround(100.0 * static_cast<double>(emb) / static_cast<double>(tot))) : 0;
      print_json(nlohmann::json{{"total_insights", tot},
                                 {"embedded", emb},
                                 {"coverage", std::to_string(pct) + "%"},
                                 {"ollama_available", oc.available()},
                                 {"model", oc.model}});
      return;
    }
    if (!oc.available()) {
      throw std::runtime_error("Ollama not available at " + oc.endpoint + " — install with: brew install ollama && ollama pull " + oc.model);
    }
    if (!emb_id.empty()) {
      auto ins = db->get_insight_by_id(emb_id);
      if (!ins) {
        throw std::runtime_error("insight " + emb_id + " not found");
      }
      auto vec = oc.embed(ins->content);
      db->update_embedding(emb_id, mnemon::serialize_vector(vec));
      db->log_op("embed", emb_id, "dim=" + std::to_string(vec.size()) + " model=" + oc.model);
      print_json(
          nlohmann::json{{"status", "embedded"}, {"id", emb_id}, {"dimension", vec.size()}, {"model", oc.model}});
      return;
    }
    if (!emb_all) {
      throw CLI::ValidationError("specify --all to backfill, --status to check coverage, or provide an insight ID");
    }
    auto missing = db->get_insights_without_embedding(0);
    if (missing.empty()) {
      print_json(nlohmann::json{{"status", "complete"}, {"message", "all insights already have embeddings"}});
      return;
    }
    int ok = 0, bad = 0;
    for (const auto& ins : missing) {
      try {
        auto vec = oc.embed(ins.content);
        db->update_embedding(ins.id, mnemon::serialize_vector(vec));
        ok++;
      } catch (...) {
        bad++;
      }
    }
    db->log_op("embed:backfill", "", "succeeded=" + std::to_string(ok) + " failed=" + std::to_string(bad) + " model=" + oc.model);
    print_json(nlohmann::json{
        {"status", "backfill_complete"}, {"succeeded", ok}, {"failed", bad}, {"model", oc.model}});
  });

  // --- status ---
  auto* status = app.add_subcommand("status", "Statistics");
  status->callback([&] {
    auto db = open_db();
    auto st = db->get_stats();
    nlohmann::json by;
    for (const auto& [k, v] : st.by_category) {
      by[k] = v;
    }
    nlohmann::json te = nlohmann::json::array();
    for (const auto& e : st.top_entities) {
      te.push_back({{"entity", e.entity}, {"count", e.count}});
    }
    int64_t sz = 0;
    if (fs::exists(db->path())) {
      std::error_code ec;
      sz = static_cast<int64_t>(fs::file_size(db->path(), ec));
    }
    print_json(nlohmann::json{{"total_insights", st.total},
                              {"deleted_insights", st.deleted_count},
                              {"by_category", by},
                              {"edge_count", st.edge_count},
                              {"top_entities", te},
                              {"oplog_count", st.oplog_count},
                              {"db_path", fs::absolute(db->path()).string()},
                              {"db_size_bytes", sz}});
  });

  // --- log ---
  auto* logc = app.add_subcommand("log", "Operation log");
  int log_limit = 20;
  logc->add_option("--limit", log_limit);
  logc->callback([&] {
    require_positive_limit("--limit", log_limit);
    auto db = open_db();
    auto entries = db->get_oplog(log_limit);
    if (entries.empty()) {
      std::cout << "No operations recorded yet.\n";
      return;
    }
    std::cout << "TIME                  OP        INSIGHT   DETAIL\n";
    std::cout << "----                  --        -------   ------\n";
    for (const auto& e : entries) {
      std::string ins = e.insight_id;
      if (ins.size() > 8) {
        ins = ins.substr(0, 8);
      }
      std::string det = e.detail;
      if (det.size() > 60) {
        det = det.substr(0, 57) + "...";
      }
      std::cout << e.created_at << "  " << std::left << std::setw(8) << e.operation << "  " << std::setw(8) << ins
                << "  " << det << "\n";
    }
  });

  // --- store ---
  auto* store = app.add_subcommand("store", "Store management");
  auto* st_list = store->add_subcommand("list", "List stores");
  st_list->callback([&] {
    auto names = mnemon::paths::list_stores(g_data_dir);
    if (names.empty()) {
      std::cout << "  (no stores yet — run 'mnemon store create <name>' or any command to create default)\n";
      return;
    }
    std::string active = resolve_store();
    for (const auto& n : names) {
      std::string marker = (n == active) ? "* " : "  ";
      std::cout << marker << n << "\n";
    }
  });
  auto* st_create = store->add_subcommand("create", "Create store");
  std::string st_name;
  st_create->add_option("name", st_name)->required();
  st_create->callback([&] {
    if (!mnemon::paths::valid_store_name(st_name)) {
      throw CLI::ValidationError("invalid store name \"" + st_name + "\": must match [a-zA-Z0-9][a-zA-Z0-9_-]*");
    }
    if (mnemon::paths::store_exists(g_data_dir, st_name)) {
      throw CLI::ValidationError("store \"" + st_name + "\" already exists");
    }
    std::string dir = mnemon::paths::store_dir(g_data_dir, st_name);
    auto db = mnemon::Database::open_readwrite(dir);
    db.reset();
    std::cout << "Created store \"" << st_name << "\"\n";
  });
  auto* st_set = store->add_subcommand("set", "Set active store");
  std::string st_setname;
  st_set->add_option("name", st_setname)->required();
  st_set->callback([&] {
    if (!mnemon::paths::valid_store_name(st_setname)) {
      throw CLI::ValidationError("invalid store name \"" + st_setname + "\": must match [a-zA-Z0-9][a-zA-Z0-9_-]*");
    }
    if (!mnemon::paths::store_exists(g_data_dir, st_setname)) {
      throw std::runtime_error("store \"" + st_setname + "\" does not exist (use 'mnemon store create " + st_setname +
                              "' first)");
    }
    if (!mnemon::paths::write_active(g_data_dir, st_setname)) {
      throw std::runtime_error("write active file");
    }
    std::cout << "Active store set to \"" << st_setname << "\"\n";
  });
  auto* st_rem = store->add_subcommand("remove", "Remove store");
  std::string st_remname;
  st_rem->add_option("name", st_remname)->required();
  st_rem->callback([&] {
    if (!mnemon::paths::valid_store_name(st_remname)) {
      throw CLI::ValidationError("invalid store name \"" + st_remname + "\": must match [a-zA-Z0-9][a-zA-Z0-9_-]*");
    }
    if (!mnemon::paths::store_exists(g_data_dir, st_remname)) {
      throw std::runtime_error("store \"" + st_remname + "\" does not exist");
    }
    if (st_remname == resolve_store()) {
      throw std::runtime_error("cannot remove the active store \"" + st_remname +
                              "\" (switch first with 'mnemon store set <other>')");
    }
    fs::remove_all(mnemon::paths::store_dir(g_data_dir, st_remname));
    std::cout << "Removed store \"" << st_remname << "\"\n";
  });

  // --- setup ---
  auto* setup = app.add_subcommand("setup", "Deploy mnemon into LLM CLI environments");
  std::string setup_target;
  bool setup_eject = false;
  bool setup_yes = false;
  bool setup_global = false;
  setup->add_option("--target", setup_target)->description("claude-code | openclaw");
  setup->add_flag("--eject", setup_eject);
  setup->add_flag("--yes", setup_yes);
  setup->add_flag("--global", setup_global);
  setup->callback([&] {
    mnemon::setup::RunOptions ro;
    ro.data_dir = g_data_dir;
    ro.version = kVersion;
    ro.target = setup_target;
    ro.eject = setup_eject;
    ro.yes = setup_yes;
    ro.global = setup_global;
    mnemon::setup::run(ro);
  });

  // --- viz ---
  auto* viz = app.add_subcommand("viz", "Export knowledge graph for visualization");
  std::string viz_format = "dot";
  std::string viz_output = "-";
  viz->add_option("--format", viz_format)->description("dot | html");
  viz->add_option("-o,--output", viz_output)->description("file (- for stdout)");
  viz->callback([&] {
    auto db = open_db();
    auto insights = db->get_all_active_insights();
    auto edges = db->get_all_edges();
    std::string out;
    if (viz_format == "dot") {
      out = mnemon::viz::render_dot(insights, edges);
    } else if (viz_format == "html") {
      out = mnemon::viz::render_html(insights, edges);
    } else {
      throw CLI::ValidationError("--format", "unsupported format (use dot or html)");
    }
    if (viz_output.empty() || viz_output == "-") {
      std::cout << out;
    } else {
      std::ofstream f(viz_output, std::ios::binary | std::ios::trunc);
      if (!f) {
        throw std::runtime_error("write viz output: " + viz_output);
      }
      f << out;
      std::cerr << "written to " << viz_output << "\n";
    }
  });

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}
