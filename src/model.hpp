#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mnemon {

using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

struct Insight {
  std::string id;
  std::string content;
  std::string category{"general"};
  int importance{3};
  std::vector<std::string> tags;
  std::vector<std::string> entities;
  std::string source{"user"};
  int access_count{0};
  TimePoint created_at{};
  TimePoint updated_at{};
  std::optional<TimePoint> deleted_at;
};

enum class EdgeType { temporal, semantic, causal, entity };

std::string edge_type_str(EdgeType t);
std::optional<EdgeType> parse_edge_type(std::string_view s);

struct Edge {
  std::string source_id;
  std::string target_id;
  EdgeType edge_type{};
  double weight{1.0};
  std::map<std::string, std::string> metadata;
  TimePoint created_at{};
};

struct EmbeddedRow {
  std::string id;
  std::string content;
  std::vector<uint8_t> embedding;
};

struct RetentionCandidate {
  Insight insight;
  double effective_importance{0};
  double days_since_access{0};
  int edge_count{0};
  bool immune{false};
};

struct OplogEntry {
  int id{0};
  std::string operation;
  std::string insight_id;
  std::string detail;
  std::string created_at;
};

struct EntityStat {
  std::string entity;
  int count{0};
};

struct InsightStats {
  int total{0};
  int deleted_count{0};
  std::map<std::string, int> by_category;
  int edge_count{0};
  std::vector<EntityStat> top_entities;
  int oplog_count{0};
};

} // namespace mnemon
