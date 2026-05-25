// On `remember`: merge extracted entities, then run all four MAGMA edge builders in deterministic order.
#include "graph_engine.hpp"

#include "db.hpp"
#include "graph_causal.hpp"
#include "graph_entity.hpp"
#include "graph_extract.hpp"
#include "graph_semantic.hpp"
#include "graph_temporal.hpp"

namespace mnemon::graph_eng {

EdgeStats on_insight_created(Database& db, Insight& insight, EmbedCache* embed_cache,
                             const std::string& entity_mode) {
  EdgeStats s;
  insight.entities = resolve_entities(insight.content, insight.entities, entity_mode);
  s.temporal = create_temporal_edges(db, insight);
  s.entity = create_entity_edges(db, insight);
  s.causal = create_causal_edges(db, insight);
  s.semantic = create_semantic_edges(db, insight, embed_cache);
  return s;
}

} // namespace mnemon::graph_eng
