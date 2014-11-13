#include "routing_session.hpp"
#include "measurement_utils.hpp"

#include "../indexer/mercator.hpp"

#include "../platform/platform.hpp"

#include "../coding/internal/file_data.hpp"


using namespace location;

namespace routing
{

static int const ON_ROUTE_MISSED_COUNT = 5;


RoutingSession::RoutingSession()
  : m_router(nullptr)
  , m_route(string())
  , m_state(RoutingNotActive)
{
}

void RoutingSession::BuildRoute(m2::PointD const & startPoint, m2::PointD const & endPoint,
                                TReadyCallbackFn const & callback)
{
  ASSERT(m_router != nullptr, ());
  m_router->SetFinalPoint(endPoint);
  RebuildRoute(startPoint, callback);
}

void RoutingSession::RebuildRoute(m2::PointD const & startPoint, TReadyCallbackFn const & callback)
{
  ASSERT(m_router != nullptr, ());
  Reset();
  m_state = RouteBuilding;

  // Use old-style callback constraction, because lambda constructs buggy function on Android
  // (callback param isn't captured by value).
  m_router->CalculateRoute(startPoint, DoReadyCallback(*this, callback));
}

void RoutingSession::DoReadyCallback::operator() (Route & route, IRouter::ResultCode e)
{
  if (e == IRouter::NoError)
    m_rs.AssignRoute(route);
  else
    m_rs.m_state = RouteNotReady;

  m_callback(m_rs.m_route, e);
}

bool RoutingSession::IsActive() const
{
  return (m_state != RoutingNotActive);
}

void RoutingSession::Reset()
{
  m_state = RoutingNotActive;
  m_lastDistance = 0.0;
  m_moveAwayCounter = 0;
  Route(string()).Swap(m_route);
}

RoutingSession::State RoutingSession::OnLocationPositionChanged(m2::PointD const & position,
                                                                GpsInfo const & info)
{
  ASSERT(m_state != RoutingNotActive, ());
  ASSERT(m_router != nullptr, ());

  if (m_state == RouteNotReady)
  {
    if (++m_moveAwayCounter > ON_ROUTE_MISSED_COUNT)
      return RouteNeedRebuild;
    else
      return RouteNotReady;
  }

  if (m_state == RouteNeedRebuild || m_state == RouteFinished || m_state == RouteBuilding)
    return m_state;

  ASSERT(m_route.IsValid(), ());

  if (m_route.MoveIterator(info))
  {
    m_moveAwayCounter = 0;
    m_lastDistance = 0.0;

    if (m_route.IsCurrentOnEnd())
      m_state = RouteFinished;
    else
      m_state = OnRoute;
  }
  else
  {
    // Distance from the last known projection on route
    // (check if we are moving far from the last known projection).
    double const dist = m_route.GetCurrentSqDistance(position);
    if (dist > m_lastDistance || my::AlmostEqual(dist, m_lastDistance, 1 << 16))
    {
      ++m_moveAwayCounter;
      m_lastDistance = dist;
    }
    else
    {
      m_moveAwayCounter = 0;
      m_lastDistance = 0.0;
    }

    if (m_moveAwayCounter > ON_ROUTE_MISSED_COUNT)
      m_state = RouteNeedRebuild;
  }

  return m_state;
}

void RoutingSession::GetRouteFollowingInfo(FollowingInfo & info) const
{
  auto formatDistFn = [](double dist, string & value, string & suffix)
  {
    /// @todo Make better formatting of distance and units.
    MeasurementUtils::FormatDistance(dist, value);

    size_t const delim = value.find(' ');
    ASSERT(delim != string::npos, ());
    suffix = value.substr(delim + 1);
    value.erase(delim);
  };

  if (m_route.IsValid())
  {
    formatDistFn(m_route.GetCurrentDistanceToEnd(), info.m_distToTarget, info.m_targetUnitsSuffix);

    {
      double dist;
      Route::TurnItem turn;
      m_route.GetTurn(dist, turn);

      formatDistFn(dist, info.m_distToTurn, info.m_turnUnitsSuffix);
      info.m_turn = turn.m_turn;
      info.m_exitNum = turn.m_exitNum;
    }

    info.m_time = m_route.GetTime();
  }
}

void RoutingSession::AssignRoute(Route & route)
{
  ASSERT(route.IsValid(), ());
  m_state = RouteNotStarted;
  m_route.Swap(route);
}

void RoutingSession::SetRouter(IRouter * router)
{
  m_router.reset(router);
}

void RoutingSession::DeleteIndexFile(string const & fileName)
{
  Reset();
  m_router->ClearState();
  (void) my::DeleteFileX(GetPlatform().WritablePathForFile(fileName));
}

}
