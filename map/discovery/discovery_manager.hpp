#pragma once

#include "search/city_finder.hpp"

#include "map/discovery/discovery_client_params.hpp"
#include "map/search_api.hpp"

#include "partners_api/booking_api.hpp"
#include "partners_api/locals_api.hpp"
#include "partners_api/viator_api.hpp"

#include "platform/platform.hpp"

#include "indexer/index.hpp"

#include "geometry/point2d.hpp"
#include "geometry/rect2d.hpp"

#include "base/thread_checker.hpp"

#include <functional>
#include <string>
#include <vector>

namespace discovery
{
class Manager final
{
public:
  struct APIs
  {
    APIs(SearchAPI & search, viator::Api const & viator, locals::Api & locals)
      : m_search(search), m_viator(viator), m_locals(locals)
    {
    }

    SearchAPI & m_search;
    viator::Api const & m_viator;
    locals::Api & m_locals;
  };

  struct Params
  {
    std::string m_curency;
    std::string m_lang;
    size_t m_itemsCount = 0;
    m2::PointD m_viewportCenter;
    m2::RectD m_viewport;
    ItemTypes m_itemTypes;
  };

  using ErrorCalback = std::function<void(ItemType const type)>;

  Manager(Index const & index, search::CityFinder & cityFinder, APIs const & apis);

  template <typename ResultCallback>
  void Discover(Params && params, ResultCallback const & onResult, ErrorCalback const & onError)
  {
    ASSERT_THREAD_CHECKER(m_threadChecker, ());
    auto const & types = params.m_itemTypes;
    ASSERT(!types.empty(), ("Types must contain at least one element."));

    for (auto const type : types)
    {
      switch (type)
      {
      case ItemType::Viator:
      {
        std::string const sponsoredId = GetCityViatorId(params.m_viewportCenter);
        if (sponsoredId.empty())
        {
          onError(type);
          break;
        }

        m_viatorApi.GetTop5Products(
            sponsoredId, params.m_curency,
            [this, sponsoredId, onResult](std::string const & destId,
                                                std::vector<viator::Product> const & products) {
              ASSERT_THREAD_CHECKER(m_threadChecker, ());
              if (destId == sponsoredId)
                onResult(products);
            });
        break;
      }
      case ItemType::Attractions: // fallthrough
      case ItemType::Cafes:
      {
        auto p = GetSearchParams(params, type);
        p.m_onResults = [onResult, type](search::Results const & results) {
          if (!results.IsEndMarker())
            return;
          GetPlatform().RunTask(Platform::Thread::Gui,
                                [onResult, type, results] { onResult(results, type); });
        };
        m_searchApi.GetEngine().Search(p);
        break;
      }
      case ItemType::Hotels:
      {
        ASSERT(false, ("Discovering hotels isn't supported yet."));
        break;
      }
      case ItemType::LocalExperts:
      {
        auto const latLon = MercatorBounds::ToLatLon(params.m_viewportCenter);
        auto constexpr pageNumber = 1;
        m_localsApi.GetLocals(
            latLon.lat, latLon.lon, params.m_lang, params.m_itemsCount, pageNumber,
            [this, onResult](uint64_t id, std::vector<locals::LocalExpert> const & locals,
                                   size_t /* pageNumber */, size_t /* countPerPage */,
                                   bool /* hasPreviousPage */, bool /* hasNextPage */) {
              ASSERT_THREAD_CHECKER(m_threadChecker, ());
              onResult(locals);
            },
            [this, onError, type](uint64_t id, int errorCode, std::string const & errorMessage) {
              ASSERT_THREAD_CHECKER(m_threadChecker, ());
              onError(type);
            });
        break;
      }
      }
    }
  }

private:
  static search::SearchParams GetSearchParams(Params const & params, ItemType const type);
  std::string GetCityViatorId(m2::PointD const & point) const;

  Index const & m_index;
  search::CityFinder & m_cityFinder;
  SearchAPI & m_searchApi;
  viator::Api const & m_viatorApi;
  locals::Api & m_localsApi;
  ThreadChecker m_threadChecker;
};
}  // namespace discovery