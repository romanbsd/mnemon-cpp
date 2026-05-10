// Graphviz DOT + self-contained HTML/vis-network for `mnemon viz` (labels escaped, category/edge colors).
#include "viz.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <unordered_set>

namespace mnemon::viz {

static std::string node_label(const Insight& i) {
  std::string content = i.content;
  if (content.size() > 60) {
    content.resize(60);
    content += "...";
  }
  return "[" + i.category + "] " + content;
}

static std::string category_color(const std::string& c) {
  if (c == "decision") {
    return "#e74c3c";
  }
  if (c == "fact") {
    return "#3498db";
  }
  if (c == "insight") {
    return "#9b59b6";
  }
  if (c == "preference") {
    return "#2ecc71";
  }
  if (c == "context") {
    return "#f39c12";
  }
  return "#95a5a6";
}

static std::string edge_color(EdgeType t) {
  switch (t) {
  case EdgeType::temporal:
    return "#aaaaaa";
  case EdgeType::semantic:
    return "#3498db";
  case EdgeType::causal:
    return "#e74c3c";
  case EdgeType::entity:
    return "#2ecc71";
  }
  return "#cccccc";
}

static std::string dot_escape(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\n') {
      out += ' ';
    } else {
      out += ch;
    }
  }
  return out;
}

static std::string js_str(std::string_view s) {
  return nlohmann::json(std::string(s)).dump();
}

std::string render_dot(const std::vector<Insight>& insights, const std::vector<Edge>& edges) {
  std::ostringstream b;
  b << "digraph mnemon {\n";
  b << "  rankdir=LR;\n";
  b << "  node [shape=box, style=\"filled,rounded\", fontsize=10, fontname=\"Helvetica\"];\n";
  b << "  edge [fontsize=8, fontname=\"Helvetica\"];\n\n";

  std::unordered_set<std::string> active;
  for (const auto& i : insights) {
    active.insert(i.id);
  }

  for (const auto& i : insights) {
    std::string label = dot_escape(node_label(i));
    std::string short_id = i.id;
    if (short_id.size() > 8) {
      short_id.resize(8);
    }
    std::string color = category_color(i.category);
    b << "  \"" << i.id << "\" [label=\"" << short_id << ": " << label << "\", fillcolor=\"" << color
      << "\", fontcolor=\"white\"];\n";
  }

  b << "\n";
  for (const auto& e : edges) {
    if (!active.count(e.source_id) || !active.count(e.target_id)) {
      continue;
    }
    std::string color = edge_color(e.edge_type);
    std::string edge_label = edge_type_str(e.edge_type);
    auto it = e.metadata.find("sub_type");
    if (it != e.metadata.end() && !it->second.empty()) {
      edge_label = it->second;
    }
    b << "  \"" << e.source_id << "\" -> \"" << e.target_id << "\" [label=\"" << dot_escape(edge_label)
      << "\", color=\"" << color << "\", fontcolor=\"" << color << "\"];\n";
  }
  b << "}\n";
  return b.str();
}

static constexpr const char* kHtmlTemplate = R"(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Mnemon Knowledge Graph</title>
<script src="https://unpkg.com/vis-network/standalone/umd/vis-network.min.js"></script>
<style>
  body { margin: 0; padding: 0; background: #1a1a2e; font-family: sans-serif; }
  #graph { width: 100vw; height: 100vh; }
  #legend { position: fixed; top: 10px; right: 10px; background: rgba(0,0,0,0.7);
    color: white; padding: 12px; border-radius: 8px; font-size: 12px; }
  .leg-item { display: flex; align-items: center; margin: 4px 0; }
  .leg-dot { width: 12px; height: 12px; border-radius: 50%; margin-right: 8px; }
  .leg-line { width: 20px; height: 3px; margin-right: 8px; }
</style>
</head>
<body>
<div id="graph"></div>
<div id="legend">
  <b>Nodes</b>
  <div class="leg-item"><div class="leg-dot" style="background:#e74c3c"></div>decision</div>
  <div class="leg-item"><div class="leg-dot" style="background:#3498db"></div>fact</div>
  <div class="leg-item"><div class="leg-dot" style="background:#9b59b6"></div>insight</div>
  <div class="leg-item"><div class="leg-dot" style="background:#2ecc71"></div>preference</div>
  <div class="leg-item"><div class="leg-dot" style="background:#f39c12"></div>context</div>
  <div class="leg-item"><div class="leg-dot" style="background:#95a5a6"></div>general</div>
  <br><b>Edges</b>
  <div class="leg-item"><div class="leg-line" style="background:#aaaaaa"></div>temporal</div>
  <div class="leg-item"><div class="leg-line" style="background:#3498db"></div>semantic</div>
  <div class="leg-item"><div class="leg-line" style="background:#e74c3c"></div>causal</div>
  <div class="leg-item"><div class="leg-line" style="background:#2ecc71"></div>entity</div>
</div>
<script>
var nodes = new vis.DataSet([%s]);
var edges = new vis.DataSet([%s]);
var container = document.getElementById("graph");
var data = { nodes: nodes, edges: edges };
var options = {
  physics: { solver: "forceAtlas2Based", forceAtlas2Based: { gravitationalConstant: -30 } },
  interaction: { hover: true, tooltipDelay: 100 },
  nodes: { shape: "box", margin: 8, borderWidth: 0, font: { size: 11 } },
  edges: { smooth: { type: "continuous" }, font: { size: 9 } }
};
new vis.Network(container, data, options);
</script>
</body>
</html>)";

std::string render_html(const std::vector<Insight>& insights, const std::vector<Edge>& edges) {
  std::unordered_set<std::string> active;
  for (const auto& i : insights) {
    active.insert(i.id);
  }

  std::ostringstream nodes;
  for (size_t idx = 0; idx < insights.size(); ++idx) {
    if (idx > 0) {
      nodes << ",\n";
    }
    const auto& i = insights[idx];
    std::string short_id = i.id;
    if (short_id.size() > 8) {
      short_id.resize(8);
    }
    std::string label = node_label(i);
    for (char& ch : label) {
      if (ch == '\n') {
        ch = ' ';
      }
    }
    std::string color = category_color(i.category);
    nodes << "{id:" << js_str(i.id) << ",label:" << js_str(short_id + ": " + label) << ",title:" << js_str(i.content)
          << ",color:" << js_str(color) << ",font:{color:\"white\"}}";
  }

  std::ostringstream edges_js;
  bool first = true;
  for (const auto& e : edges) {
    if (!active.count(e.source_id) || !active.count(e.target_id)) {
      continue;
    }
    if (!first) {
      edges_js << ",\n";
    }
    first = false;
    std::string color = edge_color(e.edge_type);
    std::string edge_label = edge_type_str(e.edge_type);
    auto it = e.metadata.find("sub_type");
    if (it != e.metadata.end() && !it->second.empty()) {
      edge_label = it->second;
    }
    edges_js << "{from:" << js_str(e.source_id) << ",to:" << js_str(e.target_id) << ",label:" << js_str(edge_label)
             << ",color:{color:" << js_str(color) << "},arrows:\"to\",font:{color:" << js_str(color) << ",size:10}}";
  }

  std::string n = nodes.str();
  std::string ed = edges_js.str();
  std::string t(kHtmlTemplate);
  size_t p = t.find("%s");
  if (p != std::string::npos) {
    t.replace(p, 2, n);
  }
  p = t.find("%s");
  if (p != std::string::npos) {
    t.replace(p, 2, ed);
  }
  return t;
}

} // namespace mnemon::viz
