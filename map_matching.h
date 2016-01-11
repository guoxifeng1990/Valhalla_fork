// -*- mode: c++ -*-

#include <algorithm>

#include <valhalla/midgard/pointll.h>
#include <valhalla/baldr/pathlocation.h>

#include <valhalla/sif/autocost.h>
#include <valhalla/sif/bicyclecost.h>
#include <valhalla/sif/pedestriancost.h>

#include "costings.h"
#include "viterbi_search.h"
#include "edge_search.h"
#include "sp.h"
#include "graph_helpers.h"
#include "geometry_helpers.h"

namespace mm {

using namespace valhalla;
using ptree = boost::property_tree::ptree;


class Measurement {
 public:
  Measurement(const PointLL& lnglat)
      : lnglat_(lnglat) {}

  const PointLL& lnglat() const
  { return lnglat_; }

 private:
  PointLL lnglat_;
};


class State
{
 public:
  State(const StateId id,
        const Time time,
        const Candidate& candidate)
      : id_(id),
        time_(time),
        candidate_(candidate),
        labelset_(nullptr),
        label_idx_() {}

  const StateId id() const
  { return id_; }

  const Time time() const
  { return time_; }

  const Candidate& candidate() const
  { return candidate_; }

  bool routed() const
  { return labelset_ != nullptr; }

  void route(const std::vector<const State*>& states,
             GraphReader& graphreader,
             float max_route_distance,
             sif::cost_ptr_t costing,
             std::shared_ptr<const sif::EdgeLabel> edgelabel,
             const float turn_cost_table[181]) const
  {
    // TODO disable routing to interpolated states

    // Prepare locations
    std::vector<PathLocation> locations;
    locations.reserve(1 + states.size());
    locations.push_back(candidate_);
    for (const auto state : states) {
      locations.push_back(state->candidate());
    }

    // Route
    labelset_ = std::make_shared<LabelSet>(std::ceil(max_route_distance));
    // TODO pass labelset_ as shared_ptr
    const auto& results = find_shortest_path(
        graphreader, locations, 0, *labelset_, costing,
        edgelabel, turn_cost_table);

    // Cache results
    label_idx_.clear();
    uint16_t dest = 1;  // dest at 0 is remained for the origin
    for (const auto state : states) {
      const auto it = results.find(dest);
      if (it != results.end()) {
        label_idx_[state->id()] = it->second;
      }
      dest++;
    }
  }

  const Label* last_label(const State& state) const
  {
    const auto it = label_idx_.find(state.id());
    if (it != label_idx_.end()) {
      return &labelset_->label(it->second);
    }
    return nullptr;
  }

  RoutePathIterator RouteBegin(const State& state) const
  {
    const auto it = label_idx_.find(state.id());
    if (it != label_idx_.end()) {
      return RoutePathIterator(labelset_.get(), it->second);
    }
    return RoutePathIterator(labelset_.get());
  }

  RoutePathIterator RouteEnd() const
  { return RoutePathIterator(labelset_.get()); }

 private:
  const StateId id_;

  const Time time_;

  const Candidate candidate_;

  mutable std::shared_ptr<LabelSet> labelset_;

  mutable std::unordered_map<StateId, uint32_t> label_idx_;
};


inline float GreatCircleDistance(const State& left,
                                 const State& right)
{
  const auto &left_pt = left.candidate().vertex(),
            &right_pt = right.candidate().vertex();
  return left_pt.Distance(right_pt);
}


inline float GreatCircleDistanceSquared(const State& left,
                                        const State& right)
{
  const auto &left_pt = left.candidate().vertex(),
            &right_pt = right.candidate().vertex();
  return left_pt.DistanceSquared(right_pt);
}


inline float GreatCircleDistance(const Measurement& left,
                                 const Measurement& right)
{ return left.lnglat().Distance(right.lnglat()); }


inline float GreatCircleDistanceSquared(const Measurement& left,
                                        const Measurement& right)
{ return left.lnglat().DistanceSquared(right.lnglat()); }


class MapMatching: public ViterbiSearch<State>
{
 public:
  // TODO move params down
  MapMatching(baldr::GraphReader& graphreader,
              const sif::cost_ptr_t* mode_costing,
              const sif::TravelMode mode,
              float sigma_z,
              float beta,
              float breakage_distance,
              float max_route_distance_factor,
              float turn_penalty_factor)
      : graphreader_(graphreader),
        mode_costing_(mode_costing),
        mode_(mode),
        measurements_(),
        states_(),
        sigma_z_(sigma_z),
        inv_double_sq_sigma_z_(1.f / (sigma_z_ * sigma_z_ * 2.f)),
        beta_(beta),
        inv_beta_(1.f / beta_),
        breakage_distance_(breakage_distance),
        max_route_distance_factor_(max_route_distance_factor),
        turn_penalty_factor_(turn_penalty_factor),
        turn_cost_table_{0.f}
  {
    if (sigma_z_ <= 0.f) {
      throw std::invalid_argument("Expect sigma_z to be positive");
    }

    if (beta_ <= 0.f) {
      throw std::invalid_argument("Expect beta to be positive");
    }

#ifndef NDEBUG
    for (size_t i = 0; i <= 180; ++i) {
      assert(!turn_cost_table_[i]);
    }
#endif

    if (0.f < turn_penalty_factor_) {
      for (int i = 0; i <= 180; ++i) {
        turn_cost_table_[i] = turn_penalty_factor_ * std::exp(-i/45.f);
      }
    } else if (turn_penalty_factor_ < 0.f) {
      throw std::invalid_argument("Expect turn penalty factor to be nonnegative");
    }
  }

  MapMatching(baldr::GraphReader& graphreader,
              const sif::cost_ptr_t* mode_costing,
              const sif::TravelMode mode,
              const ptree& config)
      : MapMatching(graphreader, mode_costing, mode,
                    config.get<float>("sigma_z"),
                    config.get<float>("beta"),
                    config.get<float>("breakage_distance"),
                    config.get<float>("max_route_distance_factor"),
                    config.get<float>("turn_penalty_factor")) {}

  virtual ~MapMatching()
  { Clear(); }

  void Clear()
  {
    measurements_.clear();
    states_.clear();
    ViterbiSearch<State>::Clear();
  }

  template <typename candidate_iterator_t>
  Time AppendState(const Measurement& measurement,
                   candidate_iterator_t begin,
                   candidate_iterator_t end)
  {
    Time time = states_.size();

    // Append to base class
    std::vector<const State*> column;
    for (auto it = begin; it != end; it++) {
      StateId id = state_.size();
      state_.push_back(new State(id, time, *it));
      column.push_back(state_.back());
    }
    unreached_states_.push_back(column);

    states_.push_back(column);
    measurements_.push_back(measurement);

    return time;
  }

  baldr::GraphReader& graphreader() const
  { return graphreader_; }

  sif::cost_ptr_t costing() const
  { return mode_costing_[static_cast<size_t>(mode_)]; }

  const std::vector<const State*>&
  states(Time time) const
  { return states_[time]; }

  const Measurement& measurement(Time time) const
  { return measurements_[time]; }

  const Measurement& measurement(const State& state) const
  { return measurements_[state.time()]; }

  std::vector<Measurement>::size_type size() const
  { return measurements_.size(); }

 protected:
  virtual float MaxRouteDistance(const State& left, const State& right) const
  {
    auto mmt_distance = GreatCircleDistance(measurement(left), measurement(right));
    return std::min(mmt_distance * max_route_distance_factor_, breakage_distance_);
  }

  float TransitionCost(const State& left, const State& right) const override
  {
    if (!left.routed()) {
      std::shared_ptr<const sif::EdgeLabel> edgelabel;
      const auto prev_stateid = predecessor(left.id());
      if (prev_stateid != kInvalidStateId) {
        const auto& prev_state = state(prev_stateid);
        assert(prev_state.routed());
        const auto label = prev_state.last_label(left);
        edgelabel = label? label->edgelabel : nullptr;
      } else {
        edgelabel = nullptr;
      }
      left.route(unreached_states_[right.time()], graphreader_,
                 MaxRouteDistance(left, right),
                 costing(), edgelabel, turn_cost_table_);
    }
    assert(left.routed());

    const auto label = left.last_label(right);
    if (label) {
      const auto mmt_distance = GreatCircleDistance(measurement(left), measurement(right));
      return (label->turn_cost + std::abs(label->cost - mmt_distance)) * inv_beta_;
    }

    assert(IsInvalidCost(-1.f));
    return -1.f;
  }

  float EmissionCost(const State& state) const override
  { return state.candidate().sq_distance() * inv_double_sq_sigma_z_; }

  double CostSofar(double prev_costsofar, float transition_cost, float emission_cost) const override
  { return prev_costsofar + transition_cost + emission_cost; }

 private:

  baldr::GraphReader& graphreader_;

  const sif::cost_ptr_t* mode_costing_;

  const TravelMode mode_;

  std::vector<Measurement> measurements_;

  std::vector<std::vector<const State*>> states_;

  float sigma_z_;
  double inv_double_sq_sigma_z_;  // equals to 1.f / (sigma_z_ * sigma_z_ * 2.f)

  float beta_;
  float inv_beta_;  // equals to 1.f / beta_

  float breakage_distance_;

  float max_route_distance_factor_;

  float turn_penalty_factor_;

  // Cost for each degree in [0, 180]
  float turn_cost_table_[181];
};


enum class GraphType: uint8_t
{ kUnknown = 0, kEdge, kNode };


class MatchResult
{
 public:
  MatchResult(const Point& lnglat,
              float distance,
              const GraphId graphid,
              GraphType graphtype,
              const State* state = nullptr)
      : lnglat_(lnglat),
        distance_(distance),
        graphid_(graphid),
        graphtype_(graphtype),
        state_(state) {}

  MatchResult(const Point& lnglat)
      : lnglat_(lnglat),
        distance_(0.f),
        graphid_(),
        graphtype_(GraphType::kUnknown),
        state_(nullptr)
  { assert(!graphid_.Is_Valid()); }

  // Coordinate of the matched point
  const PointLL& lnglat() const
  { return lnglat_; }

  // Distance from measurement to the matched point
  float distance() const
  { return distance_; }

  // Which edge/node this matched point stays
  const GraphId graphid() const
  { return graphid_; }

  GraphType graphtype() const
  { return graphtype_; }

  // Attach the state pointer for other information (e.g. reconstruct
  // the route path) and debugging
  const State* state() const
  { return state_; }

 private:
  PointLL lnglat_;
  float distance_;
  GraphId graphid_;
  GraphType graphtype_;
  const State* state_;
};


// Collect a nodeid set of a path location
std::unordered_set<GraphId>
collect_nodes(GraphReader& reader, const Candidate& location)
{
  std::unordered_set<GraphId> results;

  for (const auto& edge : location.edges()) {
    if (!edge.id.Is_Valid()) continue;
    if (edge.dist == 0.f) {
      const auto opp_edge = reader.GetOpposingEdge(edge.id);
      if (opp_edge) {
        results.insert(opp_edge->endnode());
      }
    } else if (edge.dist == 1.f) {
      const auto tile = reader.GetGraphTile(edge.id);
      if (tile) {
        const auto directededge = tile->directededge(edge.id);
        if (directededge) {
          results.insert(directededge->endnode());
        }
      }
    }
  }

  return results;
}


MatchResult
guess_source_result(const MapMatching::state_iterator source,
                    const MapMatching::state_iterator target,
                    const Measurement& source_measurement)
{
  if (source.IsValid() && target.IsValid()) {
    GraphId last_valid_id;
    GraphType last_valid_type = GraphType::kUnknown;
    for (auto label = source->RouteBegin(*target);
         label != source->RouteEnd(); label++) {
      if (label->nodeid.Is_Valid()) {
        last_valid_id = label->nodeid;
        last_valid_type = GraphType::kNode;
      } else if (label->edgeid.Is_Valid()) {
        last_valid_id = label->edgeid;
        last_valid_type = GraphType::kEdge;
      }
    }
    const auto& c = source->candidate();
    return {c.vertex(), c.distance(), last_valid_id, last_valid_type, &(*source)};
  } else if (source.IsValid()) {
    return {source_measurement.lnglat(), 0.f, GraphId(), GraphType::kUnknown, &(*source)};
  }

  return {source_measurement.lnglat()};
}


MatchResult
guess_target_result(const MapMatching::state_iterator source,
                    const MapMatching::state_iterator target,
                    const Measurement& target_measurement)
{
  if (source.IsValid() && target.IsValid()) {
    auto label = source->RouteBegin(*target);
    GraphId graphid;
    GraphType graphtype = GraphType::kUnknown;
    if (label != source->RouteEnd()) {
      if (label->nodeid.Is_Valid()) {
        graphid = label->nodeid;
        graphtype = GraphType::kNode;
      } else if (label->edgeid.Is_Valid()) {
        graphid = label->edgeid;
        graphtype = GraphType::kEdge;
      }
    }
    const auto& c = target->candidate();
    return {c.vertex(), c.distance(), graphid, graphtype, &(*target)};
  } else if (target.IsValid()) {
    return {target_measurement.lnglat(), 0.f, GraphId(), GraphType::kUnknown, &(*target)};
  }

  return {target_measurement.lnglat()};
}


template <typename candidate_iterator_t>
MatchResult
interpolate(GraphReader& reader,
            const std::unordered_set<GraphId>& graphset,
            candidate_iterator_t begin,
            candidate_iterator_t end,
            const Measurement& measurement)
{
  auto closest_candidate = end;
  float closest_sq_distance = std::numeric_limits<float>::infinity();
  GraphId closest_graphid;
  GraphType closest_graphtype = GraphType::kUnknown;

  for (auto candidate = begin; candidate != end; candidate++) {
    if (candidate->sq_distance() < closest_sq_distance) {
      if (!candidate->IsNode()) {
        for (const auto& edge : candidate->edges()) {
          const auto it = graphset.find(edge.id);
          if (it != graphset.end()) {
            closest_candidate = candidate;
            closest_sq_distance = candidate->sq_distance();
            closest_graphid = edge.id;
            closest_graphtype = GraphType::kEdge;
          }
        }
      } else {
        for (const auto nodeid : collect_nodes(reader, *candidate)) {
          const auto it = graphset.find(nodeid);
          if (it != graphset.end()) {
            closest_candidate = candidate;
            closest_sq_distance = candidate->sq_distance();
            closest_graphid = nodeid;
            closest_graphtype = GraphType::kNode;
          }
        }
      }
    }
  }

  if (closest_candidate != end) {
    return {closest_candidate->vertex(), closest_candidate->distance(), closest_graphid, closest_graphtype};
  }

  return {measurement.lnglat()};
}


std::unordered_set<GraphId>
collect_graphset(GraphReader& reader,
                 const MapMatching::state_iterator source,
                 const MapMatching::state_iterator target)
{
  std::unordered_set<GraphId> graphset;
  if (source.IsValid() && target.IsValid()) {
    for (auto label = source->RouteBegin(*target);
         label != source->RouteEnd();
         ++label) {
      if (label->edgeid.Is_Valid()) {
        graphset.insert(label->edgeid);
      }
      if (label->nodeid.Is_Valid()) {
        graphset.insert(label->nodeid);
      }
    }
  } else if (source.IsValid()) {
    const auto& location = source->candidate();
    if (!location.IsNode()) {
      for (const auto& edge : location.edges()) {
        if (edge.id.Is_Valid()) {
          graphset.insert(edge.id);
        }
      }
    } else {
      for (const auto nodeid : collect_nodes(reader, location)) {
        if (nodeid.Is_Valid()) {
          graphset.insert(nodeid);
        }
      }
    }
  }

  return graphset;
}


std::vector<MatchResult>
OfflineMatch(MapMatching& mm,
             const CandidateQuery& cq,
             const std::vector<Measurement>& measurements,
             float max_sq_search_radius,
             float interpolation_distance)
{
  mm.Clear();

  if (measurements.empty()) {
    return {};
  }

  using mmt_size_t = std::vector<Measurement>::size_type;
  Time time = 0;
  float sq_interpolation_distance = interpolation_distance * interpolation_distance;
  std::unordered_map<Time, std::vector<mmt_size_t>> proximate_measurements;

  // Load states
  for (mmt_size_t idx = 0,
             last_idx = 0,
              end_idx = measurements.size() - 1;
       idx <= end_idx; idx++) {
    const auto& measurement = measurements[idx];
    auto sq_distance = GreatCircleDistanceSquared(measurements[last_idx], measurement);
    // Always match the first and the last measurement
    if (sq_interpolation_distance <= sq_distance || idx == 0 || idx == end_idx) {
      const auto& candidates = cq.Query(measurement.lnglat(),
                                        max_sq_search_radius,
                                        mm.costing()->GetFilter());
      time = mm.AppendState(measurement, candidates.begin(), candidates.end());
      last_idx = idx;
    } else {
      proximate_measurements[time].push_back(idx);
    }
  }

  // Search viterbi path
  std::vector<MapMatching::state_iterator> iterpath;
  iterpath.reserve(mm.size());
  for (auto it = mm.SearchPath(time); it != mm.PathEnd(); it++) {
    iterpath.push_back(it);
  }
  std::reverse(iterpath.begin(), iterpath.end());
  assert(iterpath.size() == mm.size());

  // Interpolate proximate measurements and merge their states into
  // the results
  std::vector<MatchResult> results;
  results.reserve(measurements.size());
  results.emplace_back(measurements.front().lnglat());
  assert(!results.back().graphid().Is_Valid());

  for (Time time = 1; time < mm.size(); time++) {
    const auto &source_state = iterpath[time - 1],
               &target_state = iterpath[time];

    if (!results.back().graphid().Is_Valid()) {
      results.pop_back();
      results.push_back(guess_source_result(source_state, target_state, measurements[results.size()]));
    }

    auto it = proximate_measurements.find(time - 1);
    if (it != proximate_measurements.end()) {
      const auto& graphset = collect_graphset(mm.graphreader(), source_state, target_state);
      for (const auto idx : it->second) {
        const auto& candidates = cq.Query(measurements[idx].lnglat(),
                                          max_sq_search_radius,
                                          mm.costing()->GetFilter());
        results.push_back(interpolate(mm.graphreader(), graphset,
                                      candidates.begin(), candidates.end(),
                                      measurements[idx]));
      }
    }

    results.push_back(guess_target_result(source_state, target_state, measurements[results.size()]));
  }
  assert(results.size() == measurements.size());

  return results;
}


struct EdgeSegment
{
  EdgeSegment(baldr::GraphId the_edgeid,
              float the_source = 0.f,
              float the_target = 1.f)
      : edgeid(the_edgeid),
        source(the_source),
        target(the_target)
  {
    if (!(0.f <= source && source <= target && target <= 1.f)) {
      throw std::invalid_argument("Expect 0.f <= source <= source <= 1.f, but you got source = "
                                  + std::to_string(source)
                                  + " and target = "
                                  + std::to_string(target));
    }
  }

  std::vector<PointLL> Shape(baldr::GraphReader& graphreader) const
  {
    const baldr::GraphTile* tile = nullptr;
    const auto edge = helpers::edge_directededge(graphreader, edgeid, tile);
    if (edge) {
      const auto edgeinfo = tile->edgeinfo(edge->edgeinfo_offset());
      const auto& shape = edgeinfo->shape();
      if (edge->forward()) {
        return helpers::ClipLineString(shape.cbegin(), shape.cend(), source, target);
      } else {
        return helpers::ClipLineString(shape.crbegin(), shape.crend(), source, target);
      }
    }

    return {};
  }

  bool Adjoined(baldr::GraphReader& graphreader, const EdgeSegment& other) const
  {
    if (edgeid != other.edgeid) {
      if (target == 1.f && other.source == 0.f) {
        const auto endnode = helpers::edge_endnodeid(graphreader, edgeid);
        return endnode == helpers::edge_startnodeid(graphreader, other.edgeid)
            && endnode.Is_Valid();
      } else {
        return false;
      }
    } else {
      return target == other.source;
    }
  }

  // TODO make them private
  GraphId edgeid;
  float source;
  float target;
};


template <typename segment_iterator_t>
std::string
RouteToString(baldr::GraphReader& graphreader,
              segment_iterator_t segment_begin,
              segment_iterator_t segment_end,
              const baldr::GraphTile*& tile)
{
  // The string will look like: [dummy] [source/startnodeid edgeid target/endnodeid] ...
  std::ostringstream route;

  for (auto segment = segment_begin; segment != segment_end; segment++) {
    if (segment->edgeid.Is_Valid()) {
      route << "[";

      if (segment->source == 0.f) {
        route << helpers::edge_startnodeid(graphreader, segment->edgeid, tile);
      } else {
        route << segment->source;
      }

      route << " " << segment->edgeid << " ";

      if (segment->target == 1.f) {
        route << helpers::edge_endnodeid(graphreader, segment->edgeid, tile);
      } else {
        route << segment->target;
      }

      route << "]";
    } else {
      route << "[dummy]";
    }
    route << " ";
  }

  auto route_str = route.str();
  if (!route_str.empty()) {
    route_str.pop_back();
  }
  return route_str;
}


// Validate a route. It check if all edge segments of the route are
// valid and successive, and no loop
template <typename segment_iterator_t>
bool ValidateRoute(baldr::GraphReader& graphreader,
                   segment_iterator_t segment_begin,
                   segment_iterator_t segment_end,
                   const baldr::GraphTile*& tile)
{
  if (segment_begin == segment_end) {
    return true;
  }

  // The first segment must be dummy
  if (!(!segment_begin->edgeid.Is_Valid()
        && segment_begin->source == 0.f
        && segment_begin->target == 0.f)) {
    LOG_ERROR("Found the first segment's edgeid is not dummpy");
    LOG_ERROR(RouteToString(graphreader, segment_begin, segment_end, tile));
    return false;
  }

  for (auto segment = std::next(segment_begin);  // Skip the first dummy segment
       segment != segment_end; segment++) {
    // The rest of segments must have valid edgeid
    if (!segment->edgeid.Is_Valid()) {
      LOG_ERROR("Found invalid edgeid at segment " + std::to_string(segment - segment_begin));
      LOG_ERROR(RouteToString(graphreader, segment_begin, segment_end, tile));
      return false;
    }

    // Skip the first non-dummy segment
    const auto prev_segment = std::prev(segment);
    if (prev_segment == segment_begin) {
      continue;
    }

    // Successive segments must be adjacent and no loop absolutely!
    if (prev_segment->edgeid == segment->edgeid) {
      if (prev_segment->target != segment->source) {
        LOG_ERROR("Found disconnected segments at " + std::to_string(segment - segment_begin));
        LOG_ERROR(RouteToString(graphreader, segment_begin, segment_end, tile));

        // A temporary fix here: this exception is due to a few of
        // loops in the graph. The error message below is one example
        // of the fail case: the edge 2/698780/4075 is a loop since it
        // ends and starts at the same node 2/698780/1433:

        // [ERROR] Found disconnected segments at 2
        // [ERROR] [dummy] [0.816102 2/698780/4075 2/698780/1433] [2/698780/1433 2/698780/4075 0.460951]

        // We should remove this block of code when this issue is
        // solved from upstream
        const auto endnodeid = helpers::edge_endnodeid(graphreader, prev_segment->edgeid, tile);
        const auto startnodeid = helpers::edge_startnodeid(graphreader, segment->edgeid, tile);
        if (endnodeid == startnodeid) {
          LOG_ERROR("This is a loop. Let it go");
          return true;
        }
        // End of the fix

        return false;
      }
    } else {
      const auto endnodeid = helpers::edge_endnodeid(graphreader, prev_segment->edgeid, tile),
               startnodeid = helpers::edge_startnodeid(graphreader, segment->edgeid, tile);
      if (!(prev_segment->target == 1.f
            && segment->source == 0.f
            && endnodeid == startnodeid)) {
        LOG_ERROR("Found disconnected segments at " + std::to_string(segment - segment_begin));
        LOG_ERROR(RouteToString(graphreader, segment_begin, segment_end, tile));
        return false;
      }
    }
  }

  return true;
}


template <typename segment_iterator_t>
void MergeRoute(std::vector<EdgeSegment>& route,
                segment_iterator_t segment_begin,
                segment_iterator_t segment_end)
{
  if (segment_begin == segment_end) {
    return;
  }

  for (auto segment = std::next(segment_begin);  // Skip the first dummy segment
       segment != segment_end; segment++) {
    if (!segment->edgeid.Is_Valid()) {
      throw std::runtime_error("Still found an invalid edgeid in route segments");
    }
    if(!route.empty()) {
      auto& last_segment = route.back();
      if (last_segment.edgeid == segment->edgeid) {
        if (last_segment.target != segment->source
            && segment != std::next(segment_begin)) {
          // TODO should throw runtime error. See the temporary fix
          LOG_ERROR("Still found a disconnected route in which segment "
                    + std::to_string(segment - segment_begin) + " ends at "
                    + std::to_string(last_segment.target)
                    + " but the next segment starts at "
                    + std::to_string(segment->source));
        }
        // and here we should extend last_segment.target =
        // segment->target since last_segment.target <=
        // segment->target but see the temporary fix
        last_segment.target = std::max(last_segment.target, segment->target);
      } else {
        route.push_back(*segment);
      }
    } else {
      route.push_back(*segment);
    }
  }
}


template <typename match_iterator_t>
std::vector<EdgeSegment>
ConstructRoute(GraphReader& graphreader, match_iterator_t begin, match_iterator_t end)
{
  std::vector<EdgeSegment> route;
  match_iterator_t previous_match = end;
  const baldr::GraphTile* tile = nullptr;

  for (auto match = begin; match != end; match++) {
    if (match->state()) {
      if (previous_match != end) {
        std::vector<EdgeSegment> segments;
        for (auto segment = previous_match->state()->RouteBegin(*match->state()),
                      end = previous_match->state()->RouteEnd();
             segment != end; segment++) {
          segments.emplace_back(segment->edgeid, segment->source, segment->target);
        }
        if (ValidateRoute(graphreader, segments.rbegin(), segments.rend(), tile)) {
          MergeRoute(route, segments.rbegin(), segments.rend());
        } else {
          throw std::runtime_error("Found invalid route");
        }
      }
      previous_match = match;
    }
  }

  return route;
}


inline float local_tile_size(const GraphReader& graphreader)
{
  const auto& tile_hierarchy = graphreader.GetTileHierarchy();
  const auto& tiles = tile_hierarchy.levels().rbegin()->second.tiles;
  return tiles.TileSize();
}


// A facade that connects everything
class MapMatcher final
{
 public:
  MapMatcher(const ptree&,
             baldr::GraphReader&,
             CandidateGridQuery&,
             const sif::cost_ptr_t*,
             sif::TravelMode);

  ~MapMatcher();

  baldr::GraphReader& graphreader();

  CandidateQuery& rangequery();

  sif::TravelMode travelmode() const;

  const ptree config() const;

  ptree config();

  MapMatching& mapmatching();

  std::vector<MatchResult>
  OfflineMatch(const std::vector<Measurement>&);

 private:
  ptree config_;

  baldr::GraphReader& graphreader_;

  CandidateGridQuery& rangequery_;

  const sif::cost_ptr_t* mode_costing_;

  sif::TravelMode travelmode_;

  MapMatching mapmatching_;
};


MapMatcher::MapMatcher(const ptree& config,
                       baldr::GraphReader& graphreader,
                       CandidateGridQuery& rangequery,
                       const sif::cost_ptr_t* mode_costing,
                       sif::TravelMode travelmode)
    : config_(config),
      graphreader_(graphreader),
      rangequery_(rangequery),
      mode_costing_(mode_costing),
      travelmode_(travelmode),
      mapmatching_(graphreader_, mode_costing_, travelmode_, config_) {}


MapMatcher::~MapMatcher() {}


baldr::GraphReader&
MapMatcher::graphreader()
{ return graphreader_; }


CandidateQuery&
MapMatcher::rangequery()
{ return rangequery_; }


sif::TravelMode
MapMatcher::travelmode() const
{ return travelmode_; }


const ptree
MapMatcher::config() const
{ return config_; }


ptree
MapMatcher::config()
{ return config_; }


MapMatching&
MapMatcher::mapmatching()
{ return mapmatching_; }


inline std::vector<MatchResult>
MapMatcher::OfflineMatch(const std::vector<Measurement>& measurements)
{
  float search_radius = std::min(config_.get<float>("search_radius"),
                                 config_.get<float>("max_search_radius"));
  float interpolation_distance = config_.get<float>("interpolation_distance");
  return mm::OfflineMatch(mapmatching_, rangequery_, measurements,
                          search_radius * search_radius,
                          interpolation_distance);
}


namespace {
constexpr size_t kModeCostingCount = 8;
}


class MapMatcherFactory final
{
public:
  MapMatcherFactory(const ptree&);

  ~MapMatcherFactory();

  baldr::GraphReader& graphreader();

  CandidateQuery& rangequery();

  sif::TravelMode NameToTravelMode(const std::string&);

  const std::string& TravelModeToName(sif::TravelMode);

  MapMatcher* Create(sif::TravelMode);

  MapMatcher* Create(const std::string&);

  MapMatcher* Create(const ptree&);

  MapMatcher* Create(const std::string&, const ptree&);

  MapMatcher* Create(sif::TravelMode, const ptree&);

  ptree
  MergeConfig(const std::string&, const ptree&);

  ptree&
  MergeConfig(const std::string&, ptree&);

  void ClearCacheIfPossible();

  void ClearCache();

private:
  typedef sif::cost_ptr_t (*factory_function_t)(const ptree&);

  ptree config_;

  baldr::GraphReader graphreader_;

  sif::cost_ptr_t mode_costing_[kModeCostingCount];

  std::string mode_name_[kModeCostingCount];

  CandidateGridQuery rangequery_;

  float max_grid_cache_size_;

  size_t register_costing(const std::string&, factory_function_t, const ptree&);

  sif::cost_ptr_t* init_costings(const ptree&);
};


MapMatcherFactory::MapMatcherFactory(const ptree& root)
    : config_(root.get_child("mm")),
      graphreader_(root.get_child("mjolnir.hierarchy")),
      mode_costing_{nullptr},
      mode_name_(),
      rangequery_(graphreader_,
                  local_tile_size(graphreader_)/root.get<size_t>("grid.size"),
                  local_tile_size(graphreader_)/root.get<size_t>("grid.size")),
      max_grid_cache_size_(root.get<float>("grid.cache_size"))
{
#ifndef NDEBUG
  for (size_t idx = 0; idx < kModeCostingCount; idx++) {
    assert(!mode_costing_[idx]);
    assert(mode_name_[idx].empty());
  }
#endif

  init_costings(root);
}


MapMatcherFactory::~MapMatcherFactory() {}


baldr::GraphReader&
MapMatcherFactory::graphreader()
{ return graphreader_; }


CandidateQuery&
MapMatcherFactory::rangequery()
{ return rangequery_; }


sif::TravelMode
MapMatcherFactory::NameToTravelMode(const std::string& name)
{
  for (size_t idx = 0; idx < kModeCostingCount; idx++) {
    if (!name.empty() && mode_name_[idx] == name) {
      return static_cast<sif::TravelMode>(idx);
    }
  }
  throw std::invalid_argument("Invalid costing name: " + name);
}


const std::string&
MapMatcherFactory::TravelModeToName(sif::TravelMode travelmode)
{
  const auto index = static_cast<size_t>(travelmode);
  if (index < kModeCostingCount) {
    if (!mode_name_[index].empty()) {
      return mode_name_[index];
    }
  }
  throw std::invalid_argument("Invalid travelmode code " + std::to_string(index));
}


inline MapMatcher*
MapMatcherFactory::Create(sif::TravelMode travelmode)
{ return Create(travelmode, ptree()); }


inline MapMatcher*
MapMatcherFactory::Create(const std::string& name)
{ return Create(NameToTravelMode(name), ptree()); }


inline MapMatcher*
MapMatcherFactory::Create(const ptree& preferences)
{
  const auto& name = preferences.get<std::string>("mode", config_.get<std::string>("mode"));
  auto travelmode = NameToTravelMode(name);
  const auto& config = MergeConfig(name, preferences);
  return Create(travelmode, preferences);
}


inline MapMatcher*
MapMatcherFactory::Create(const std::string& name, const ptree& preferences)
{ return Create(NameToTravelMode(name), preferences); }


inline MapMatcher*
MapMatcherFactory::Create(sif::TravelMode travelmode, const ptree& preferences)
{
  const auto& config = MergeConfig(TravelModeToName(travelmode), preferences);
  // TODO investigate exception safety
  return new MapMatcher(config, graphreader_, rangequery_, mode_costing_, travelmode);
}


ptree
MapMatcherFactory::MergeConfig(const std::string& name, const ptree& preferences)
{
  // Copy the default child config
  auto config = config_.get_child("default");

  // The mode-specific config overwrites defaults
  const auto mode_config = config_.get_child_optional(name);
  if (mode_config) {
    for (const auto& child : *mode_config) {
      config.put_child(child.first, child.second);
    }
  }

  // Preferences overwrites defaults
  for (const auto& child : preferences) {
    config.put_child(child.first, child.second);
  }

  // Give it back
  return config;
}


ptree&
MapMatcherFactory::MergeConfig(const std::string& name, ptree& preferences)
{
  const auto mode_config = config_.get_child_optional(name);
  if (mode_config) {
    for (const auto& child : *mode_config) {
      auto pchild = preferences.get_child_optional(child.first);
      if (!pchild) {
        preferences.put_child(child.first, child.second);
      }
    }
  }

  for (const auto& child : config_.get_child("default")) {
    auto pchild = preferences.get_child_optional(child.first);
    if (!pchild) {
      preferences.put_child(child.first, child.second);
    }
  }

  return preferences;
}


size_t
MapMatcherFactory::register_costing(const std::string& mode_name,
                                    factory_function_t factory,
                                    const ptree& config)
{
  auto costing = factory(config);
  auto index = static_cast<size_t>(costing->travelmode());
  if (!(index < kModeCostingCount)) {
    throw std::out_of_range("Configuration error: out of bounds");
  }
  if (mode_costing_[index]) {
    throw std::runtime_error("Configuration error: found duplicate travel mode");
  }
  mode_costing_[index] = costing;
  mode_name_[index] = mode_name;
  return index;
}


sif::cost_ptr_t*
MapMatcherFactory::init_costings(const ptree& root)
{
  register_costing("auto", sif::CreateAutoCost, root.get_child("costing_options.auto"));
  register_costing("bicycle", sif::CreateBicycleCost, root.get_child("costing_options.bicycle"));
  register_costing("pedestrian", sif::CreatePedestrianCost, root.get_child("costing_options.pedestrian"));
  register_costing("multimodal", CreateUniversalCost, root.get_child("costing_options.multimodal"));

  return mode_costing_;
}


void MapMatcherFactory::ClearCacheIfPossible()
{
  if(graphreader_.OverCommitted()) {
    graphreader_.Clear();
  }

  if (rangequery_.size() > max_grid_cache_size_) {
    rangequery_.Clear();
  }
}


void MapMatcherFactory::ClearCache()
{
  graphreader_.Clear();
  rangequery_.Clear();
}


}
