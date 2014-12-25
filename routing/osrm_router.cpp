#include "osrm_router.hpp"
#include "vehicle_model.hpp"

#include "../base/math.hpp"

#include "../geometry/angles.hpp"
#include "../geometry/distance.hpp"
#include "../geometry/distance_on_sphere.hpp"

#include "../indexer/ftypes_matcher.hpp"
#include "../indexer/mercator.hpp"
#include "../indexer/index.hpp"
#include "../indexer/scales.hpp"
#include "../indexer/mwm_version.hpp"
#include "../indexer/search_string_utils.hpp"

#include "../platform/platform.hpp"

#include "../coding/reader_wrapper.hpp"

#include "../base/logging.hpp"

#include "../3party/osrm/osrm-backend/DataStructures/SearchEngineData.h"
#include "../3party/osrm/osrm-backend/Descriptors/DescriptionFactory.h"
#include "../3party/osrm/osrm-backend/RoutingAlgorithms/ShortestPathRouting.h"


namespace routing
{

size_t const MAX_NODE_CANDIDATES = 10;
double const FEATURE_BY_POINT_RADIUS_M = 1000.0;
double const TIME_OVERHEAD = 1.4;
double const FEATURES_NEAR_TURN_M = 3.0;

namespace
{
class Point2Geometry : private noncopyable
{
  m2::PointD m_p, m_p1;
  OsrmRouter::GeomTurnCandidateT & m_candidates;
public:
  Point2Geometry(m2::PointD const & p, m2::PointD const & p1,
                 OsrmRouter::GeomTurnCandidateT & candidates)
    : m_p(p), m_p1(p1), m_candidates(candidates)
  {
  }

  void operator() (FeatureType const & ft)
  {
    static CarModel const carModel;
    if (ft.GetFeatureType() != feature::GEOM_LINE || !carModel.IsRoad(ft))
      return;
    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);
    size_t const count = ft.GetPointsCount();
    ASSERT_GREATER(count, 1, ());

    auto addAngle = [&](m2::PointD const & p, m2::PointD const & p1, m2::PointD const & p2)
    {
      double const a = my::RadToDeg(ang::TwoVectorsAngle(p, p1, p2));
      if (!my::AlmostEqual(a, 0.))
        m_candidates.push_back(a);
    };

    for (size_t i = 0; i < count; ++i)
    {
      if (MercatorBounds::DistanceOnEarth(m_p, ft.GetPoint(i)) < FEATURES_NEAR_TURN_M)
      {
        if (i > 0)
          addAngle(m_p, m_p1, ft.GetPoint(i - 1));
        if (i < count - 1)
          addAngle(m_p, m_p1, ft.GetPoint(i + 1));
        return;
      }
    }
  }
};

class Point2PhantomNode : private noncopyable
{
  m2::PointD m_point;
  m2::PointD const m_direction;
  OsrmFtSegMapping const & m_mapping;

  struct Candidate
  {
    double m_dist;
    uint32_t m_segIdx;
    uint32_t m_fid;
    m2::PointD m_point;

    Candidate() : m_dist(numeric_limits<double>::max()), m_fid(OsrmFtSegMapping::FtSeg::INVALID_FID) {}
  };

  buffer_vector<Candidate, 128> m_candidates;
  uint32_t m_mwmId;
  Index const * m_pIndex;

public:
  Point2PhantomNode(OsrmFtSegMapping const & mapping, Index const * pIndex, m2::PointD const & direction)
    : m_direction(direction), m_mapping(mapping),
      m_mwmId(numeric_limits<uint32_t>::max()), m_pIndex(pIndex)
  {
  }

  void SetPoint(m2::PointD const & pt)
  {
    m_point = pt;
  }

  bool HasCandidates() const
  {
    return !m_candidates.empty();
  }

  void operator() (FeatureType const & ft)
  {
    static CarModel const carModel;
    if (ft.GetFeatureType() != feature::GEOM_LINE || !carModel.IsRoad(ft))
      return;

    Candidate res;

    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    size_t const count = ft.GetPointsCount();
    ASSERT_GREATER(count, 1, ());
    for (size_t i = 1; i < count; ++i)
    {
      /// @todo Probably, we need to get exact projection distance in meters.
      m2::ProjectionToSection<m2::PointD> segProj;
      segProj.SetBounds(ft.GetPoint(i - 1), ft.GetPoint(i));

      m2::PointD const pt = segProj(m_point);
      double const d = m_point.SquareLength(pt);
      if (d < res.m_dist)
      {
        res.m_dist = d;
        res.m_fid = ft.GetID().m_offset;
        res.m_segIdx = i - 1;
        res.m_point = pt;

        if (m_mwmId == numeric_limits<uint32_t>::max())
          m_mwmId = ft.GetID().m_mwm;
        ASSERT_EQUAL(m_mwmId, ft.GetID().m_mwm, ());
      }
    }

    if (res.m_fid != OsrmFtSegMapping::FtSeg::INVALID_FID)
      m_candidates.push_back(res);
  }

  double CalculateDistance(OsrmFtSegMapping::FtSeg const & s) const
  {
    ASSERT_NOT_EQUAL(s.m_pointStart, s.m_pointEnd, ());

    Index::FeaturesLoaderGuard loader(*m_pIndex, m_mwmId);
    FeatureType ft;
    loader.GetFeature(s.m_fid, ft);
    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    double dist = 0.0;
    size_t n = max(s.m_pointEnd, s.m_pointStart);
    size_t i = min(s.m_pointStart, s.m_pointEnd) + 1;
    do
    {
      dist += MercatorBounds::DistanceOnEarth(ft.GetPoint(i - 1), ft.GetPoint(i));
      ++i;
    } while (i <= n);

    return dist;
  }

  void CalculateOffset(OsrmFtSegMapping::FtSeg const & seg, m2::PointD const & segPt, NodeID & nodeId, int & offset, bool forward) const
  {
    if (nodeId == INVALID_NODE_ID)
      return;

    double distance = 0;
    auto const range = m_mapping.GetSegmentsRange(nodeId);
    OsrmFtSegMapping::FtSeg s, cSeg;

    int si = forward ? range.second - 1 : range.first;
    int ei = forward ? range.first - 1 : range.second;
    int di = forward ? -1 : 1;

    for (int i = si; i != ei; i += di)
    {
      m_mapping.GetSegmentByIndex(i, s);
      auto s1 = min(s.m_pointStart, s.m_pointEnd);
      auto e1 = max(s.m_pointEnd, s.m_pointStart);

      // seg.m_pointEnd - seg.m_pointStart == 1, so check
      // just a case, when seg is inside s
      if ((seg.m_pointStart != s1 || seg.m_pointEnd != e1) &&
          (s1 <= seg.m_pointStart && e1 >= seg.m_pointEnd))
      {
        cSeg.m_fid = s.m_fid;

        if (s.m_pointStart < s.m_pointEnd)
        {
          if (forward)
          {
            cSeg.m_pointEnd = seg.m_pointEnd;
            cSeg.m_pointStart = s.m_pointStart;
          }
          else
          {
            cSeg.m_pointStart = seg.m_pointStart;
            cSeg.m_pointEnd = s.m_pointEnd;
          }
        }
        else
        {
          if (forward)
          {
            cSeg.m_pointStart = s.m_pointEnd;
            cSeg.m_pointEnd = seg.m_pointEnd;
          }
          else
          {
            cSeg.m_pointEnd = seg.m_pointStart;
            cSeg.m_pointStart = s.m_pointStart;
          }
        }

        distance += CalculateDistance(cSeg);
        break;
      }
      else
        distance += CalculateDistance(s);
    }

    Index::FeaturesLoaderGuard loader(*m_pIndex, m_mwmId);
    FeatureType ft;
    loader.GetFeature(seg.m_fid, ft);
    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    // node.m_seg always forward ordered (m_pointStart < m_pointEnd)
    distance -= MercatorBounds::DistanceOnEarth(ft.GetPoint(forward ? seg.m_pointEnd : seg.m_pointStart), segPt);

    offset = max(static_cast<int>(distance), 1);
  }

  void CalculateOffsets(FeatureGraphNode & node) const
  {
    CalculateOffset(node.m_seg, node.m_segPt, node.m_node.forward_node_id, node.m_node.forward_offset, true);
    CalculateOffset(node.m_seg, node.m_segPt, node.m_node.reverse_node_id, node.m_node.reverse_offset, false);

    // need to initialize weights for correct work of PhantomNode::GetForwardWeightPlusOffset
    // and PhantomNode::GetReverseWeightPlusOffset
    node.m_node.forward_weight = 0;
    node.m_node.reverse_weight = 0;
  }

  void MakeResult(FeatureGraphNodeVecT & res, size_t maxCount, uint32_t & mwmId, volatile bool const & requestCancel)
  {
    mwmId = m_mwmId;
    if (mwmId == numeric_limits<uint32_t>::max())
      return;

    vector<OsrmFtSegMapping::FtSeg> segments;

    segments.resize(maxCount);

    OsrmFtSegMapping::FtSegSetT segmentSet;
    sort(m_candidates.begin(), m_candidates.end(), [] (Candidate const & r1, Candidate const & r2)
    {
      return (r1.m_dist < r2.m_dist);
    });

    size_t const n = min(m_candidates.size(), maxCount);
    for (size_t j = 0; j < n; ++j)
    {
      OsrmFtSegMapping::FtSeg & seg = segments[j];
      Candidate const & c = m_candidates[j];

      seg.m_fid = c.m_fid;
      seg.m_pointStart = c.m_segIdx;
      seg.m_pointEnd = c.m_segIdx + 1;

      segmentSet.insert(&seg);
    }

    OsrmFtSegMapping::OsrmNodesT nodes;
    m_mapping.GetOsrmNodes(segmentSet, nodes, requestCancel);

    res.clear();
    res.resize(maxCount);

      for (size_t j = 0; j < maxCount; ++j)
      {
        size_t const idx = j;

        if (!segments[idx].IsValid())
          continue;

        auto it = nodes.find(segments[idx].Store());
        if (it != nodes.end())
        {
          FeatureGraphNode & node = res[idx];

          if (!m_direction.IsAlmostZero())
          {
            // Filter income nodes by direction mode
            OsrmFtSegMapping::FtSeg const & node_seg = segments[idx];
            FeatureType feature;
            Index::FeaturesLoaderGuard loader(*m_pIndex, mwmId);
            loader.GetFeature(node_seg.m_fid, feature);
            feature.ParseGeometry(FeatureType::BEST_GEOMETRY);
            m2::PointD const featureDirection = feature.GetPoint(node_seg.m_pointEnd) - feature.GetPoint(node_seg.m_pointStart);
            bool const sameDirection = (m2::DotProduct(featureDirection, m_direction) / (featureDirection.Length() * m_direction.Length()) > 0);
            if (sameDirection)
            {
              node.m_node.forward_node_id = it->second.first;
              node.m_node.reverse_node_id = INVALID_NODE_ID;
            }
            else
            {
              node.m_node.forward_node_id = INVALID_NODE_ID;
              node.m_node.reverse_node_id = it->second.second;
            }
          }
          else
          {
            node.m_node.forward_node_id = it->second.first;
            node.m_node.reverse_node_id = it->second.second;
          }

          node.m_seg = segments[idx];
          node.m_segPt = m_candidates[j].m_point;

          CalculateOffsets(node);
        }
      }
  }
};

} // namespace

RoutingMapping::RoutingMapping(string fName): map_counter(0), facade_counter(0), m_base_name(fName+DATA_FILE_EXTENSION)
{
  Platform & pl = GetPlatform();
  fName += DATA_FILE_EXTENSION;

  string const fPath = pl.WritablePathForFile(fName + ROUTING_FILE_EXTENSION);
  if (!pl.IsFileExistsByFullPath(fPath))
    throw IRouter::ResultCode::RouteFileNotExist;
  // Open new container and check that mwm and routing have equal timestamp.
  LOG(LDEBUG, ("Load routing index for file:", fPath));
  m_container.Open(fPath);
  {
    FileReader r1 = m_container.GetReader(VERSION_FILE_TAG);
    ReaderSrc src1(r1);
    ModelReaderPtr r2 = FilesContainerR(pl.GetReader(fName)).GetReader(VERSION_FILE_TAG);
    ReaderSrc src2(r2.GetPtr());

    if (ver::ReadTimestamp(src1) != ver::ReadTimestamp(src2))
    {
      m_container.Close();
      throw IRouter::ResultCode::InconsistentMWMandRoute;
    }
    mapping.Load(m_container);
  }
}

OsrmRouter::OsrmRouter(Index const * index, CountryFileFnT const & fn)
  : m_pIndex(index), m_indexManager(fn), m_isFinalChanged(false),
    m_requestCancel(false)
{
  m_isReadyThread.clear();
}

string OsrmRouter::GetName() const
{
  return "mapsme";
}

void OsrmRouter::ClearState()
{
  m_requestCancel = true;

  threads::MutexGuard guard(m_routeMutex);
  UNUSED_VALUE(guard);

  m_cachedFinalNodes.clear();

  m_indexManager.Clear();
}

void OsrmRouter::SetFinalPoint(m2::PointD const & finalPt)
{
  {
    threads::MutexGuard guard(m_paramsMutex);
    UNUSED_VALUE(guard);

    m_finalPt = finalPt;
    m_isFinalChanged = true;

    m_requestCancel = true;
  }
}

void OsrmRouter::CalculateRoute(m2::PointD const & startPt, ReadyCallback const & callback, m2::PointD const & direction)
{
  {
    threads::MutexGuard guard(m_paramsMutex);
    UNUSED_VALUE(guard);

    m_startPt = startPt;
    m_startDr = direction;

    m_requestCancel = true;
  }

  GetPlatform().RunAsync(bind(&OsrmRouter::CalculateRouteAsync, this, callback));
}

void OsrmRouter::CalculateRouteAsync(ReadyCallback const & callback)
{
  if (m_isReadyThread.test_and_set())
    return;

  Route route(GetName());
  ResultCode code;

  threads::MutexGuard guard(m_routeMutex);
  UNUSED_VALUE(guard);

  m_isReadyThread.clear();

  m2::PointD startPt, finalPt, startDr;
  {
    threads::MutexGuard params(m_paramsMutex);
    UNUSED_VALUE(params);

    startPt = m_startPt;
    finalPt = m_finalPt;
    startDr = m_startDr;

    if (m_isFinalChanged)
      m_cachedFinalNodes.clear();
    m_isFinalChanged = false;

    m_requestCancel = false;
  }

  try
  {
    code = CalculateRouteImpl(startPt, startDr, finalPt, route);
    switch (code)
    {
    case StartPointNotFound:
      LOG(LWARNING, ("Can't find start point node"));
      break;
    case EndPointNotFound:
      LOG(LWARNING, ("Can't find end point node"));
      break;
    case PointsInDifferentMWM:
      LOG(LWARNING, ("Points are in different MWMs"));
      break;
    case RouteNotFound:
      LOG(LWARNING, ("Route not found"));
      break;
    case RouteFileNotExist:
      LOG(LWARNING, ("There are no routing file"));
      break;

    default:
      break;
    }
  }
  catch (Reader::Exception const & e)
  {
    LOG(LERROR, ("Routing index absent or incorrect. Error while loading routing index:", e.Msg()));
    code = InternalError;

    m_indexManager.Clear();
  }

  GetPlatform().RunOnGuiThread(bind(callback, route, code));
}

namespace
{

bool IsRouteExist(RawRouteData const & r)
{
  return !(INVALID_EDGE_WEIGHT == r.shortest_path_length ||
          r.segment_end_coordinates.empty() ||
          r.source_traversed_in_reverse.empty());
}

}

bool OsrmRouter::FindSingleRoute(FeatureGraphNodeVecT const & source, FeatureGraphNodeVecT const & target, DataFacadeT & facade,
                     RawRouteData& outputPath, FeatureGraphNodeVecT::const_iterator & sourceEdge, FeatureGraphNodeVecT::const_iterator & targetEdge)
{
  SearchEngineData engineData;
  ShortestPathRouting<DataFacadeT> pathFinder(&facade, engineData);

  sourceEdge = source.cbegin();
  targetEdge = target.cbegin();
  while (sourceEdge < source.cend() && targetEdge < target.cend())
  {
    PhantomNodes nodes;
    nodes.source_phantom = sourceEdge->m_node;
    nodes.target_phantom = targetEdge->m_node;

    outputPath = RawRouteData();

    if ((nodes.source_phantom.forward_node_id != INVALID_NODE_ID ||
         nodes.source_phantom.reverse_node_id != INVALID_NODE_ID) &&
        (nodes.target_phantom.forward_node_id != INVALID_NODE_ID ||
         nodes.target_phantom.reverse_node_id != INVALID_NODE_ID))
    {
      outputPath.segment_end_coordinates.push_back(nodes);

      pathFinder({nodes}, {}, outputPath);
    }

    /// @todo: make more complex nearest edge turnaround
    if (!IsRouteExist(outputPath))
    {
      ++sourceEdge;
      if (sourceEdge == source.cend())
      {
        ++targetEdge;
        sourceEdge = source.cbegin();
      }
    }
    else
      return true;
  }

  return IsRouteExist(outputPath);
}

OsrmRouter::ResultCode OsrmRouter::CalculateRouteImpl(m2::PointD const & startPt, m2::PointD const & startDr, m2::PointD const & finalPt, Route & route)
{
  RoutingMappingPtrT startMapping;
  RoutingMappingPtrT finalMapping;
  try
  {
    startMapping = m_indexManager.GetMappingByPoint(startPt);
    finalMapping = m_indexManager.GetMappingByPoint(finalPt);
  }
  catch (OsrmRouter::ResultCode e)
  {
      return e;
  }

  // 3. Find start/end nodes.
  MultiroutingTaskPointT startTask;

  startTask.resize(1);
  uint32_t mwmId = -1;

  {
    startMapping->Map();
    ResultCode const code = FindPhantomNodes(startMapping->GetName(), startPt, startDr, startTask[0], MAX_NODE_CANDIDATES, mwmId, startMapping->mapping);
    if (code != NoError)
      return code;
    startMapping->Unmap();
  }
  {
    if (finalPt != m_CachedTargetPoint)
    {
      m_CachedTargetTask.resize(1);
      finalMapping->Map();
      ResultCode const code = FindPhantomNodes(finalMapping->GetName(), finalPt, m2::PointD::Zero(), m_CachedTargetTask[0], MAX_NODE_CANDIDATES, mwmId, finalMapping->mapping);
      if (code != NoError)
        return code;
      m_CachedTargetPoint = finalPt;
      finalMapping->Unmap();
    }
  }
  if (m_requestCancel)
    return Cancelled;

  // 4. Find route.
  RawRouteData rawRoute;
  FeatureGraphNodeVecT::const_iterator sourceEdge;
  FeatureGraphNodeVecT::const_iterator targetEdge;

  if (startMapping->GetName() == finalMapping->GetName())
  {
    startMapping->LoadFacade();
    if (!FindSingleRoute(startTask[0], m_CachedTargetTask[0], startMapping->dataFacade, rawRoute, sourceEdge, targetEdge))
      return RouteNotFound;
  }
  if (m_requestCancel)
    return Cancelled;

  // 5. Restore route.
  startMapping->Map();

  ASSERT(sourceEdge != startTask[0].cend(), ());
  ASSERT(targetEdge != m_CachedTargetTask[0].cend(), ());

  typedef OsrmFtSegMapping::FtSeg SegT;

  FeatureGraphNode const & sNode = *sourceEdge;
  FeatureGraphNode const & eNode = *targetEdge;

  SegT const & segBegin = sNode.m_seg;
  SegT const & segEnd = eNode.m_seg;

  ASSERT(segBegin.IsValid(), ());
  ASSERT(segEnd.IsValid(), ());

  Route::TurnsT turnsDir;
  Route::TimesT times;
  double estimateTime = 0;
#ifdef _DEBUG
  size_t lastIdx = 0;
#endif

  LOG(LDEBUG, ("Length:", rawRoute.shortest_path_length));

  //! @todo: Improve last segment time calculation
  CarModel carModel;
  vector<m2::PointD> points;
  for (auto i : osrm::irange<std::size_t>(0, rawRoute.unpacked_path_segments.size()))
  {
    if (m_requestCancel)
      return Cancelled;

    // Get all the coordinates for the computed route
    size_t const n = rawRoute.unpacked_path_segments[i].size();
    for (size_t j = 0; j < n; ++j)
    {
      PathData const & path_data = rawRoute.unpacked_path_segments[i][j];

      if (j > 0)
      {
        Route::TurnItem t;
        t.m_index = points.size() - 1;

        GetTurnDirection(rawRoute.unpacked_path_segments[i][j - 1],
                         rawRoute.unpacked_path_segments[i][j],
                         mwmId, startMapping, t);
        if (t.m_turn != turns::NoTurn)
          turnsDir.push_back(t);


        // osrm multiple seconds to 10, so we need to divide it back
        double const sTime = TIME_OVERHEAD * path_data.segment_duration / 10.0;
#ifdef _DEBUG
        double dist = 0.0;
        for (size_t l = lastIdx + 1; l < points.size(); ++l)
          dist += MercatorBounds::DistanceOnEarth(points[l - 1], points[l]);
        LOG(LDEBUG, ("Speed:", 3.6 * dist / sTime, "kmph; Dist:", dist, "Time:", sTime, "s", lastIdx, "e", points.size()));
        lastIdx = points.size();
#endif
        estimateTime += sTime;
        times.push_back(Route::TimeItemT(points.size(), estimateTime));
      }

      buffer_vector<SegT, 8> buffer;
      startMapping->mapping.ForEachFtSeg(path_data.node, MakeBackInsertFunctor(buffer));

      auto correctFn = [&buffer] (SegT const & seg, size_t & ind)
      {
        auto const it = find_if(buffer.begin(), buffer.end(), [&seg] (OsrmFtSegMapping::FtSeg const & s)
        {
          return s.IsIntersect(seg);
        });

        ASSERT(it != buffer.end(), ());
        ind = distance(buffer.begin(), it);
      };

      //m_mapping.DumpSegmentByNode(path_data.node);

      size_t startK = 0, endK = buffer.size();
      if (j == 0)
        correctFn(segBegin, startK);
      if (j == n - 1)
      {
        correctFn(segEnd, endK);
        ++endK;
      }

      for (size_t k = startK; k < endK; ++k)
      {
        SegT const & seg = buffer[k];

        FeatureType ft;
        Index::FeaturesLoaderGuard loader(*m_pIndex, mwmId);
        loader.GetFeature(seg.m_fid, ft);
        ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

        auto startIdx = seg.m_pointStart;
        auto endIdx = seg.m_pointEnd;
        bool const needTime = (j == 0) || (j == n - 1);

        if (j == 0 && k == startK)
          startIdx = (seg.m_pointEnd > seg.m_pointStart) ? segBegin.m_pointStart : segBegin.m_pointEnd;
        if (j == n - 1 && k == endK - 1)
          endIdx = (seg.m_pointEnd > seg.m_pointStart) ? segEnd.m_pointEnd : segEnd.m_pointStart;

        if (seg.m_pointEnd > seg.m_pointStart)
        {
          for (auto idx = startIdx; idx <= endIdx; ++idx)
          {
            points.push_back(ft.GetPoint(idx));
            if (needTime && idx > startIdx)
              estimateTime += MercatorBounds::DistanceOnEarth(ft.GetPoint(idx - 1), ft.GetPoint(idx)) / carModel.GetSpeed(ft);
          }
        }
        else
        {
          for (auto idx = startIdx; idx > endIdx; --idx)
          {
            if (needTime)
              estimateTime += MercatorBounds::DistanceOnEarth(ft.GetPoint(idx - 1), ft.GetPoint(idx)) / carModel.GetSpeed(ft);
            points.push_back(ft.GetPoint(idx));
          }
          points.push_back(ft.GetPoint(endIdx));
        }
      }
    }
  }

  points.front() = sNode.m_segPt;
  points.back() = eNode.m_segPt;

  if (points.size() < 2)
    return RouteNotFound;

  times.push_back(Route::TimeItemT(points.size() - 1, estimateTime));
  turnsDir.push_back(Route::TurnItem(points.size() - 1, turns::ReachedYourDestination));
  FixupTurns(points, turnsDir);

  turns::TurnsGeomT turnsGeom;
  CalculateTurnGeometry(points, turnsDir, turnsGeom);

#ifdef _DEBUG
  for (auto t : turnsDir)
  {
    LOG(LDEBUG, (turns::GetTurnString(t.m_turn), ":", t.m_index, t.m_srcName, "-", t.m_trgName, "exit:", t.m_exitNum));
  }

  size_t last = 0;
  double lastTime = 0;
  for (Route::TimeItemT t : times)
  {
    double dist = 0;
    for (size_t i = last + 1; i <= t.first; ++i)
      dist += MercatorBounds::DistanceOnEarth(points[i - 1], points[i]);

    double time = t.second - lastTime;

    LOG(LDEBUG, ("distance:", dist, "start:", last, "end:", t.first, "Time:", time, "Speed:", 3.6 * dist / time));
    last = t.first;
    lastTime = t.second;
  }
#endif

  route.SetGeometry(points.begin(), points.end());
  route.SetTurnInstructions(turnsDir);
  route.SetSectionTimes(times);
  route.SetTurnInstructionsGeometry(turnsGeom);

  LOG(LDEBUG, ("Estimate time:", estimateTime, "s"));

  return NoError;
}
m2::PointD OsrmRouter::GetPointForTurnAngle(OsrmFtSegMapping::FtSeg const &seg,
                                            FeatureType const &ft, m2::PointD const &turnPnt,
                                            size_t (*GetPndInd)(const size_t, const size_t, const size_t)) const
{
  const size_t maxPntsNum = 7;
  const double maxDistMeter = 300.f;
  double curDist = 0.f;
  m2::PointD pnt = turnPnt, nextPnt;

  const size_t segDist = abs(seg.m_pointEnd - seg.m_pointStart);
  ASSERT_LESS(segDist, ft.GetPointsCount(), ());
  const size_t usedFtPntNum = min(maxPntsNum, segDist);

  for (size_t i = 1; i <= usedFtPntNum; ++i)
  {
    nextPnt = ft.GetPoint(GetPndInd(seg.m_pointStart, seg.m_pointEnd, i));
    curDist += MercatorBounds::DistanceOnEarth(pnt, nextPnt);
    if (curDist > maxDistMeter)
    {
      return nextPnt;
    }
    pnt = nextPnt;
  }
  return nextPnt;
}

NodeID OsrmRouter::GetTurnTargetNode(NodeID src, NodeID trg, QueryEdge::EdgeData const & edgeData, RoutingMappingPtrT const & routingMapping)
{
  ASSERT_NOT_EQUAL(src, SPECIAL_NODEID, ());
  ASSERT_NOT_EQUAL(trg, SPECIAL_NODEID, ());
  if (!edgeData.shortcut)
    return trg;

  ASSERT_LESS(edgeData.id, routingMapping->dataFacade.GetNumberOfNodes(), ());
  EdgeID edge = SPECIAL_EDGEID;
  QueryEdge::EdgeData d;
  for (EdgeID e : routingMapping->dataFacade.GetAdjacentEdgeRange(edgeData.id))
  {
    if (routingMapping->dataFacade.GetTarget(e) == src )
    {
      d = routingMapping->dataFacade.GetEdgeData(e, edgeData.id);
      if (d.backward)
      {
        edge = e;
        break;
      }
    }
  }

  if (edge == SPECIAL_EDGEID)
  {
    for (EdgeID e : routingMapping->dataFacade.GetAdjacentEdgeRange(src))
    {
      if (routingMapping->dataFacade.GetTarget(e) == edgeData.id)
      {
        d = routingMapping->dataFacade.GetEdgeData(e, src);
        if (d.forward)
        {
          edge = e;
          break;
        }
      }
    }
  }
  ASSERT_NOT_EQUAL(edge, SPECIAL_EDGEID, ());

  if (d.shortcut)
    return GetTurnTargetNode(src, edgeData.id, d, routingMapping);

  return edgeData.id;
}

void OsrmRouter::GetPossibleTurns(NodeID node,
                                  m2::PointD const & p1,
                                  m2::PointD const & p,
                                  uint32_t mwmId,
                                  RoutingMappingPtrT const & routingMapping,
                                  OsrmRouter::TurnCandidatesT & candidates)
{

  for (EdgeID e : routingMapping->dataFacade.GetAdjacentEdgeRange(node))
  {
    QueryEdge::EdgeData const data = routingMapping->dataFacade.GetEdgeData(e, node);
    if (!data.forward)
      continue;

    NodeID trg = GetTurnTargetNode(node, routingMapping->dataFacade.GetTarget(e), data, routingMapping);
    ASSERT_NOT_EQUAL(trg, SPECIAL_NODEID, ());

    auto const range = routingMapping->mapping.GetSegmentsRange(trg);
    OsrmFtSegMapping::FtSeg seg;
    routingMapping->mapping.GetSegmentByIndex(range.first, seg);

    FeatureType ft;
    Index::FeaturesLoaderGuard loader(*m_pIndex, mwmId);
    loader.GetFeature(seg.m_fid, ft);
    ft.ParseGeometry(FeatureType::BEST_GEOMETRY);

    m2::PointD const p2 = ft.GetPoint(seg.m_pointStart < seg.m_pointEnd ? seg.m_pointStart + 1 : seg.m_pointStart - 1);
    ASSERT_EQUAL(p, ft.GetPoint(seg.m_pointStart), ());

    double const a = my::RadToDeg(ang::TwoVectorsAngle(p, p1, p2));

    candidates.emplace_back(a, trg);
  }

  sort(candidates.begin(), candidates.end(), [](TurnCandidate const & t1, TurnCandidate const & t2)
  {
    return t1.m_node < t2.m_node;
  });

  auto last = unique(candidates.begin(), candidates.end(), [](TurnCandidate const & t1, TurnCandidate const & t2)
  {
    return t1.m_node == t2.m_node;
  });
  candidates.erase(last, candidates.end());

  sort(candidates.begin(), candidates.end(), [](TurnCandidate const & t1, TurnCandidate const & t2)
  {
    return t1.m_angle < t2.m_angle;
  });
}

void OsrmRouter::GetTurnGeometry(m2::PointD const & p, m2::PointD const & p1,
                                 OsrmRouter::GeomTurnCandidateT & candidates, string const & fName) const
{
  Point2Geometry getter(p, p1, candidates);
  m_pIndex->ForEachInRectForMWM(getter, MercatorBounds::RectByCenterXYAndSizeInMeters(p, FEATURES_NEAR_TURN_M),
                                scales::GetUpperScale(), fName);
}

turns::TurnDirection OsrmRouter::InvertDirection(turns::TurnDirection dir) const
{
  switch (dir)
  {
  case turns::TurnSharpRight:
    return turns::TurnSharpLeft;
  case turns::TurnRight:
    return turns::TurnLeft;
  case turns::TurnSlightRight:
    return turns::TurnSlightLeft;
  case turns::TurnSlightLeft:
    return turns::TurnSlightRight;
  case turns::TurnLeft:
    return turns::TurnRight;
  case turns::TurnSharpLeft:
    return turns::TurnSharpRight;
  default:
    return dir;
  };
}

turns::TurnDirection OsrmRouter::MostRightDirection(const double angle) const
{
  double const lowerSharpRightBound = 23.;
  double const upperSharpRightBound = 67.;
  double const upperRightBound = 140.;
  double const upperSlightRight = 195.;
  double const upperGoStraitBound = 205.;
  double const upperSlightLeftBound = 240.;
  double const upperLeftBound = 336.;

  if (angle >= lowerSharpRightBound && angle < upperSharpRightBound)
    return turns::TurnSharpRight;
  else if (angle >= upperSharpRightBound && angle < upperRightBound)
    return  turns::TurnRight;
  else if (angle >= upperRightBound && angle < upperSlightRight)
    return turns::TurnSlightRight;
  else if (angle >= upperSlightRight && angle < upperGoStraitBound)
    return  turns::GoStraight;
  else if (angle >= upperGoStraitBound && angle < upperSlightLeftBound)
    return  turns::TurnSlightLeft;
  else if (angle >= upperSlightLeftBound && angle < upperLeftBound)
    return  turns::TurnLeft;
  return turns::NoTurn;
}

turns::TurnDirection OsrmRouter::MostLeftDirection(const double angle) const
{
  return InvertDirection(MostRightDirection(360 - angle));
}

turns::TurnDirection OsrmRouter::IntermediateDirection(const double angle) const
{
  double const lowerSharpRightBound = 23.;
  double const upperSharpRightBound = 67.;
  double const upperRightBound = 130.;
  double const upperSlightRight = 170.;
  double const upperGoStraitBound = 190.;
  double const upperSlightLeftBound = 230.;
  double const upperLeftBound = 292.;
  double const upperSharpLeftBound = 336.;

  if (angle >= lowerSharpRightBound && angle < upperSharpRightBound)
    return turns::TurnSharpRight;
  else if (angle >= upperSharpRightBound && angle < upperRightBound)
    return  turns::TurnRight;
  else if (angle >= upperRightBound && angle < upperSlightRight)
    return turns::TurnSlightRight;
  else if (angle >= upperSlightRight && angle < upperGoStraitBound)
    return  turns::GoStraight;
  else if (angle >= upperGoStraitBound && angle < upperSlightLeftBound)
    return  turns::TurnSlightLeft;
  else if (angle >= upperSlightLeftBound && angle < upperLeftBound)
    return  turns::TurnLeft;
  else if (angle >= upperLeftBound && angle < upperSharpLeftBound)
    return turns::TurnSharpLeft;
  return turns::NoTurn;
}

bool OsrmRouter::KeepOnewayOutgoingTurnIncomingEdges(TurnCandidatesT const & nodes, Route::TurnItem const & turn,
                            m2::PointD const & p, m2::PointD const & p1OneSeg, string const & fName) const
{
  size_t const outgoingNotesCount = 1;
  if (turns::IsGoStraightOrSlightTurn(turn.m_turn))
    return false;
  else
  {
    GeomTurnCandidateT geoNodes;
    GetTurnGeometry(p, p1OneSeg, geoNodes, fName);
    if (geoNodes.size() <= outgoingNotesCount)
      return false;
    return true;
  }
}

bool OsrmRouter::KeepOnewayOutgoingTurnRoundabout(bool isRound1, bool isRound2) const
{
  return !isRound1 && isRound2;
}

turns::TurnDirection OsrmRouter::RoundaboutDirection(bool isRound1, bool isRound2,
                                         bool hasMultiTurns, Route::TurnItem const & turn) const
{
  if (isRound1 && isRound2)
  {
    if (hasMultiTurns)
      return turns::StayOnRoundAbout;
    else
      return turns::NoTurn;
  }

  if (!isRound1 && isRound2)
    return turns::EnterRoundAbout;

  if (isRound1 && !isRound2)
    return turns::LeaveRoundAbout;

  ASSERT(false, ());
  return turns::NoTurn;
}

void OsrmRouter::GetTurnDirection(PathData const & node1,
                                  PathData const & node2,
                                  uint32_t mwmId, RoutingMappingPtrT const & routingMapping, Route::TurnItem & turn)
{
  auto nSegs1 = routingMapping->mapping.GetSegmentsRange(node1.node);
  auto nSegs2 = routingMapping->mapping.GetSegmentsRange(node2.node);

  ASSERT_GREATER(nSegs1.second, 0, ());

  OsrmFtSegMapping::FtSeg seg1, seg2;
  routingMapping->mapping.GetSegmentByIndex(nSegs1.second - 1, seg1);
  routingMapping->mapping.GetSegmentByIndex(nSegs2.first, seg2);

  FeatureType ft1, ft2;
  Index::FeaturesLoaderGuard loader1(*m_pIndex, mwmId);
  Index::FeaturesLoaderGuard loader2(*m_pIndex, mwmId);

  loader1.GetFeature(seg1.m_fid, ft1);
  loader2.GetFeature(seg2.m_fid, ft2);

  ft1.ParseGeometry(FeatureType::BEST_GEOMETRY);
  ft2.ParseGeometry(FeatureType::BEST_GEOMETRY);

  ASSERT_LESS(MercatorBounds::DistanceOnEarth(ft1.GetPoint(seg1.m_pointEnd), ft2.GetPoint(seg2.m_pointStart)), 2, ());

  m2::PointD const p = ft1.GetPoint(seg1.m_pointEnd);
  m2::PointD const p1 = GetPointForTurnAngle(seg1, ft1, p,
                    [](const size_t start, const size_t end, const size_t i)
                    {
                      return end > start ? end - i : end + i;
                    });
  m2::PointD const p2 = GetPointForTurnAngle(seg2, ft2, p,
                    [](const size_t start, const size_t end, const size_t i)
                    {
                      return end > start ? start + i : start - i;
                    });
  double const a = my::RadToDeg(ang::TwoVectorsAngle(p, p1, p2));

  m2::PointD const p1OneSeg = ft1.GetPoint(seg1.m_pointStart < seg1.m_pointEnd ? seg1.m_pointEnd - 1 : seg1.m_pointEnd + 1);
  TurnCandidatesT nodes;
  GetPossibleTurns(node1.node, p1OneSeg, p, mwmId, routingMapping, nodes);

#ifdef _DEBUG
  GeomTurnCandidateT geoNodes;
  GetTurnGeometry(p, p1OneSeg, geoNodes, routingMapping->GetName());

  m2::PointD const p2OneSeg = ft2.GetPoint(seg2.m_pointStart < seg2.m_pointEnd ? seg2.m_pointStart + 1 : seg2.m_pointStart - 1);

  double const aOneSeg = my::RadToDeg(ang::TwoVectorsAngle(p, p1OneSeg, p2OneSeg));
  LOG(LDEBUG, ("Possible turns. nodes = ", nodes.size(), ". ang = ", aOneSeg, ". node = ", node2.node));
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    TurnCandidate const &t = nodes[i];
    LOG(LDEBUG, ("Angle:", t.m_angle, "Node:", t.m_node));
  }
#endif

  turn.m_turn = turns::NoTurn;
  size_t const nodesSz = nodes.size();
  bool const hasMultiTurns = (nodesSz >= 2);

  if (nodesSz == 0)
  {
    ASSERT(false, ());
    return;
  }

  if (nodes.front().m_node == node2.node)
    turn.m_turn = MostRightDirection(a);
  else if (nodes.back().m_node == node2.node)
    turn.m_turn = MostLeftDirection(a);
  else turn.m_turn = IntermediateDirection(a);

  bool const isRound1 = ftypes::IsRoundAboutChecker::Instance()(ft1);
  bool const isRound2 = ftypes::IsRoundAboutChecker::Instance()(ft2);

  if (!hasMultiTurns
      && !KeepOnewayOutgoingTurnIncomingEdges(nodes, turn, p, p1OneSeg, routingMapping->GetName())
      && !KeepOnewayOutgoingTurnRoundabout(isRound1, isRound2))
  {
    turn.m_turn = turns::NoTurn;
    return;
  }

  if (isRound1 || isRound2)
  {
    turn.m_turn = RoundaboutDirection(isRound1, isRound2, hasMultiTurns, turn);
    return;
  }

  turn.m_keepAnyway = (!ftypes::IsLinkChecker::Instance()(ft1)
                       && ftypes::IsLinkChecker::Instance()(ft2));

  // get names
  string name1, name2;
  {
    ft1.GetName(FeatureType::DEFAULT_LANG, turn.m_srcName);
    ft2.GetName(FeatureType::DEFAULT_LANG, turn.m_trgName);

    search::GetStreetNameAsKey(turn.m_srcName, name1);
    search::GetStreetNameAsKey(turn.m_trgName, name2);
  }

  string road1 = ft1.GetRoadNumber();
  string road2 = ft2.GetRoadNumber();

  if (!turn.m_keepAnyway
      && ((!name1.empty() && name1 == name2) || (!road1.empty() && road1 == road2)))
  {
    turn.m_turn = turns::NoTurn;
    return;
  }

  if (turn.m_turn == turns::GoStraight)
  {
    if (!hasMultiTurns)
      turn.m_turn = turns::NoTurn;

    return;
  }

  if (turn.m_turn == turns::NoTurn)
    turn.m_turn = turns::UTurn;
}

void OsrmRouter::CalculateTurnGeometry(vector<m2::PointD> const & points, Route::TurnsT const & turnsDir, turns::TurnsGeomT & turnsGeom) const
{
  size_t const pointsSz = points.size();
  for (Route::TurnItem const & t : turnsDir)
  {
    ASSERT(t.m_index < pointsSz, ());
    if (t.m_index == 0 || t.m_index == (pointsSz - 1))
      continue;

    uint32_t const beforePivotCount = 10;
    /// afterPivotCount is more because there are half body and the arrow after the pivot point
    uint32_t const afterPivotCount = beforePivotCount + 10;
    uint32_t const fromIndex = (t.m_index <= beforePivotCount) ? 0 : t.m_index - beforePivotCount;
    uint32_t const toIndex = min<uint32_t>(pointsSz, t.m_index + afterPivotCount);
    uint32_t const turnIndex = (t.m_index <= beforePivotCount ? t.m_index : beforePivotCount);
    turnsGeom.emplace_back(t.m_index, turnIndex, points.begin() + fromIndex, points.begin() + toIndex);
  }
}

void OsrmRouter::FixupTurns(vector<m2::PointD> const & points, Route::TurnsT & turnsDir) const
{
  uint32_t exitNum = 0;
  Route::TurnItem * roundabout = 0;

  auto distance = [&points](uint32_t start, uint32_t end)
  {
    double res = 0.0;
    for (uint32_t i = start + 1; i < end; ++i)
      res += MercatorBounds::DistanceOnEarth(points[i - 1], points[i]);
    return res;
  };

  for (uint32_t idx = 0; idx < turnsDir.size(); )
  {
    Route::TurnItem & t = turnsDir[idx];
    if (roundabout && t.m_turn != turns::StayOnRoundAbout && t.m_turn != turns::LeaveRoundAbout)
    {
      exitNum = 0;
      roundabout = 0;
    }
    else if (t.m_turn == turns::EnterRoundAbout)
    {
      ASSERT_EQUAL(roundabout, 0, ());
      roundabout = &t;
    }
    else if (t.m_turn == turns::StayOnRoundAbout)
    {
      ++exitNum;
      turnsDir.erase(turnsDir.begin() + idx);
      continue;
    }
    else if (roundabout && t.m_turn == turns::LeaveRoundAbout)
    {
      roundabout->m_exitNum = exitNum + 1;
      roundabout = 0;
      exitNum = 0;
    }

    double const mergeDist = 30.0;

    if (idx > 0 &&
        turns::IsStayOnRoad(turnsDir[idx - 1].m_turn) &&
        turns::IsLeftOrRightTurn(turnsDir[idx].m_turn) &&
        distance(turnsDir[idx - 1].m_index, turnsDir[idx].m_index) < mergeDist)
    {
      turnsDir.erase(turnsDir.begin() + idx - 1);
      continue;
    }

    if (!t.m_keepAnyway
        && turns::IsGoStraightOrSlightTurn(t.m_turn)
        && !t.m_srcName.empty()
        && strings::AlmostEqual(t.m_srcName, t.m_trgName, 2))
    {
      turnsDir.erase(turnsDir.begin() + idx);
      continue;
    }

    ++idx;
  }
}

IRouter::ResultCode OsrmRouter::FindPhantomNodes(string const & fName, m2::PointD const & point, m2::PointD const & direction,
                                                 FeatureGraphNodeVecT & res, size_t maxCount, uint32_t & mwmId, OsrmFtSegMapping const & mapping)
{
  Point2PhantomNode getter(mapping, m_pIndex, direction);
  getter.SetPoint(point);

  LOG(LINFO, ("MAKE FOR EACH OF", fName));
  m_pIndex->ForEachInRectForMWM(getter,
    MercatorBounds::RectByCenterXYAndSizeInMeters(point, FEATURE_BY_POINT_RADIUS_M),
    scales::GetUpperScale(), fName);

  if (!getter.HasCandidates())
    return StartPointNotFound;

  getter.MakeResult(res, maxCount, mwmId, m_requestCancel);
  return NoError;
}

}
