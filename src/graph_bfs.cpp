// Undirected adjacency for traversal: each stored edge is walked from both endpoints (`related` / exploration).
#include "graph_bfs.hpp"

#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace mnemon::graph_eng {

std::vector<BFSNode> bfs(Database& db, const std::string& start_id, const BFSOptions& opts) {
  auto all_insights = db.get_all_active_insights();
  std::unordered_map<std::string, Insight> imap;
  for (const auto& i : all_insights) {
    imap[i.id] = i;
  }
  auto all_edges = db.get_all_edges();
  std::unordered_map<std::string, std::vector<Edge>> adj;
  for (const auto& e : all_edges) {
    adj[e.source_id].push_back(e);
    if (e.source_id != e.target_id) {
      adj[e.target_id].push_back(e);
    }
  }

  struct Entry {
    std::string id;
    int hop;
  };
  std::unordered_set<std::string> visited;
  visited.insert(start_id);
  std::deque<Entry> queue;
  queue.push_back({start_id, 0});
  std::vector<BFSNode> result;

  while (!queue.empty()) {
    if (opts.max_nodes > 0 && static_cast<int>(result.size()) >= opts.max_nodes) {
      break;
    }
    Entry cur = queue.front();
    queue.pop_front();
    if (cur.hop >= opts.max_depth) {
      continue;
    }
    for (const auto& edge : adj[cur.id]) {
      if (opts.edge_filter && edge.edge_type != *opts.edge_filter) {
        continue;
      }
      std::string neighbor = edge.target_id;
      if (neighbor == cur.id) {
        neighbor = edge.source_id;
      }
      if (visited.count(neighbor)) {
        continue;
      }
      visited.insert(neighbor);
      auto it = imap.find(neighbor);
      if (it == imap.end()) {
        continue;
      }
      result.push_back(BFSNode{it->second, cur.hop + 1, edge});
      if (opts.max_nodes > 0 && static_cast<int>(result.size()) >= opts.max_nodes) {
        break;
      }
      queue.push_back({neighbor, cur.hop + 1});
    }
  }
  return result;
}

} // namespace mnemon::graph_eng
