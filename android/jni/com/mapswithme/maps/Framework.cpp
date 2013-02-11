#include "Framework.hpp"
#include "VideoTimer.hpp"

#include "../core/jni_helper.hpp"
#include "../core/render_context.hpp"

#include "../../../../../map/framework.hpp"

#include "../../../../../gui/controller.hpp"

#include "../../../../../indexer/drawing_rules.hpp"

#include "../../../../../coding/file_container.hpp"
#include "../../../../../coding/file_name_utils.hpp"

#include "../../../../../graphics/opengl/framebuffer.hpp"
#include "../../../../../graphics/opengl/opengl.hpp"

#include "../../../../../platform/platform.hpp"
#include "../../../../../platform/location.hpp"

#include "../../../../../base/math.hpp"
#include "../../../../../base/logging.hpp"

#include "../../../../../std/shared_ptr.hpp"
#include "../../../../../std/bind.hpp"


#define LONG_CLICK_LENGTH_SEC 1.0
#define SHORT_CLICK_LENGTH_SEC 0.5


android::Framework * g_framework = 0;

namespace android
{
  void Framework::CallRepaint()
  {
    //LOG(LINFO, ("Calling Repaint"));
  }

  Framework::Framework()
   : m_work(),
     m_eventType(NVMultiTouchEventType(0)),
     m_hasFirst(false),
     m_hasSecond(false),
     m_mask(0),
     m_isInsideDoubleClick(false),
     m_isCleanSingleClick(false),
     m_doLoadState(true),
     m_onClickFnsHandler(0)
  {
    ASSERT_EQUAL ( g_framework, 0, () );
    g_framework = this;

    m_videoTimer = new VideoTimer(bind(&Framework::CallRepaint, this));
    size_t const measurementsCount = 5;
    m_sensors[0].SetCount(measurementsCount);
    m_sensors[1].SetCount(measurementsCount);
  }

  Framework::~Framework()
  {
    delete m_videoTimer;
  }

  void Framework::OnLocationError(int errorCode)
  {
    m_work.OnLocationError(static_cast<location::TLocationError>(errorCode));
  }

  void Framework::OnLocationUpdated(uint64_t time, double lat, double lon, float accuracy)
  {
    location::GpsInfo info;
    info.m_timestamp = static_cast<double>(time);
    info.m_latitude = lat;
    info.m_longitude = lon;
    info.m_horizontalAccuracy = accuracy;
    m_work.OnLocationUpdate(info);
  }

  void Framework::OnCompassUpdated(uint64_t timestamp, double magneticNorth, double trueNorth, double accuracy)
  {
    location::CompassInfo info;
    info.m_timestamp = static_cast<double>(timestamp);
    info.m_magneticHeading = magneticNorth;
    info.m_trueHeading = trueNorth;
    info.m_accuracy = accuracy;
    m_work.OnCompassUpdate(info);
  }

  void Framework::UpdateCompassSensor(int ind, float * arr)
  {
    //LOG ( LINFO, ("Sensors before, C++: ", arr[0], arr[1], arr[2]) );
    m_sensors[ind].Next(arr);
    //LOG ( LINFO, ("Sensors after, C++: ", arr[0], arr[1], arr[2]) );
  }

  void Framework::DeleteRenderPolicy()
  {
    m_work.SaveState();
    LOG(LINFO, ("Clearing current render policy."));
    m_work.SetRenderPolicy(0);
    m_work.EnterBackground();
  }

  bool Framework::InitRenderPolicy(int densityDpi, int screenWidth, int screenHeight)
  {
    graphics::ResourceManager::Params rmParams;

    rmParams.m_videoMemoryLimit = 30 * 1024 * 1024;
    rmParams.m_rtFormat = graphics::Data8Bpp;
    rmParams.m_texFormat = graphics::Data4Bpp;

    RenderPolicy::Params rpParams;

    rpParams.m_videoTimer = m_videoTimer;
    rpParams.m_useDefaultFB = true;
    rpParams.m_rmParams = rmParams;
    rpParams.m_primaryRC = make_shared_ptr(new android::RenderContext());

    char const * suffix = 0;
    switch (densityDpi)
    {
    case 120:
      rpParams.m_density = graphics::EDensityLDPI;
      break;

    case 160:
      rpParams.m_density = graphics::EDensityMDPI;
      break;

    case 240:
      rpParams.m_density = graphics::EDensityHDPI;
      break;

    default:
      rpParams.m_density = graphics::EDensityXHDPI;
      break;
    }

    rpParams.m_skinName = "basic.skn";
    LOG(LINFO, ("Using", graphics::convert(rpParams.m_density), "resources"));

    rpParams.m_screenWidth = screenWidth;
    rpParams.m_screenHeight = screenHeight;

    try
    {
      m_work.SetRenderPolicy(CreateRenderPolicy(rpParams));
      if (m_doLoadState)
        LoadState();
      else
        m_doLoadState = true;
    }
    catch (graphics::gl::platform_unsupported const & e)
    {
      LOG(LINFO, ("This android platform is unsupported, reason:", e.what()));
      return false;
    }

    m_work.SetUpdatesEnabled(true);
    m_work.EnterForeground();
    return true;
  }

  storage::Storage & Framework::Storage()
  {
    return m_work.Storage();
  }

  CountryStatusDisplay * Framework::GetCountryStatusDisplay()
  {
    return m_work.GetCountryStatusDisplay();
  }

  void Framework::ShowCountry(storage::TIndex const & idx)
  {
    m_doLoadState = false;

    m_work.ShowCountry(idx);
  }

  storage::TStatus Framework::GetCountryStatus(storage::TIndex const & idx) const
  {
    return m_work.GetCountryStatus(idx);
  }

  void Framework::DeleteCountry(storage::TIndex const & idx)
  {
    m_work.DeleteCountry(idx);
  }

  void Framework::Resize(int w, int h)
  {
    m_work.OnSize(w, h);
  }

  void Framework::DrawFrame()
  {
    if (m_work.NeedRedraw())
    {
      m_work.SetNeedRedraw(false);

      shared_ptr<PaintEvent> paintEvent(new PaintEvent(m_work.GetRenderPolicy()->GetDrawer().get()));

      m_work.BeginPaint(paintEvent);
      m_work.DoPaint(paintEvent);

      NVEventSwapBuffersEGL();

      m_work.EndPaint(paintEvent);
    }
  }

  void Framework::Move(int mode, double x, double y)
  {
    DragEvent e(x, y);
    switch (mode)
    {
    case 0: m_work.StartDrag(e); break;
    case 1: m_work.DoDrag(e); break;
    case 2: m_work.StopDrag(e); break;
    }
  }

  void Framework::Zoom(int mode, double x1, double y1, double x2, double y2)
  {
    ScaleEvent e(x1, y1, x2, y2);
    switch (mode)
    {
    case 0: m_work.StartScale(e); break;
    case 1: m_work.DoScale(e); break;
    case 2: m_work.StopScale(e); break;
    }
  }

  void Framework::KillLongTouchTask()
  {
    if (m_scheduledTask)
    {
      m_scheduledTask->Cancel();
      m_scheduledTask.reset();
    }
  }

  void Framework::Touch(int action, int mask, double x1, double y1, double x2, double y2)
  {
    NVMultiTouchEventType eventType = (NVMultiTouchEventType)action;

    // processing double-click
    if ((mask != 0x1) || (eventType == NV_MULTITOUCH_CANCEL))
    {
      if (mask == 0x1)
        m_work.GetGuiController()->OnTapCancelled(m2::PointD(x1, y1));

      // cancelling double click
      m_isInsideDoubleClick = false;
      m_isCleanSingleClick = false;
      KillLongTouchTask();
    }
    else
    {
      if (eventType == NV_MULTITOUCH_DOWN)
      {
        m_isCleanSingleClick = true;
        m_lastX1 = x1;
        m_lastY1 = y1;

        if (m_work.GetGuiController()->OnTapStarted(m2::PointD(x1, y1)))
          return;
        m_scheduledTask.reset(new ScheduledTask(bind(
                    & android::Framework::CallLongClickListener,
                    this,
                    static_cast<double>(x1),
                    static_cast<double>(y1)),
                    static_cast<int>(LONG_CLICK_LENGTH_SEC * 1000)
                    ));
        m_longClickTimer.Reset();
      }

      if (eventType == NV_MULTITOUCH_MOVE)
      {
        double k = m_work.GetRenderPolicy()->VisualScale();
        if ((fabs(x1 - m_lastX1) > 10 * k)
        ||  (fabs(y1 - m_lastY1) > 10 * k))
        {
          m_isCleanSingleClick = false;
          KillLongTouchTask();
        }

        if (m_work.GetGuiController()->OnTapMoved(m2::PointD(x1, y1)))
          return;
      }
      if (eventType == NV_MULTITOUCH_UP)
        if (m_work.GetGuiController()->OnTapEnded(m2::PointD(x1, y1)))
          return;

      if ((eventType == NV_MULTITOUCH_UP) && (m_isCleanSingleClick))
      {
        double timerTime = m_longClickTimer.ElapsedSeconds();
        KillLongTouchTask();
        if (timerTime < SHORT_CLICK_LENGTH_SEC)
        {
          CallClickListener(static_cast<int>(x1), static_cast<int>(y1));
        }
        if (m_work.GetGuiController()->OnTapEnded(m2::PointD(x1, y1)))
          return;

        if (m_isInsideDoubleClick)
        {
          if (m_doubleClickTimer.ElapsedSeconds() <= 0.5)
          {
            // performing double-click
            m_isInsideDoubleClick = false;
            m_work.ScaleToPoint(ScaleToPointEvent(x1, y1, 1.5));
          }
          else
          {
            // restarting double click
            m_isInsideDoubleClick = true;
            m_doubleClickTimer.Reset();
          }
        }
        else
        {
          // starting double click
          m_isInsideDoubleClick = true;
          m_doubleClickTimer.Reset();
        }
      }
    }

    // general case processing
    if (m_mask != mask)
    {
      if (m_mask == 0x0)
      {
        if (mask == 0x1)
          m_work.StartDrag(DragEvent(x1, y1));

        if (mask == 0x2)
          m_work.StartDrag(DragEvent(x2, y2));

        if (mask == 0x3)
          m_work.StartScale(ScaleEvent(x1, y1, x2, y2));
      }

      if (m_mask == 0x1)
      {
        m_work.StopDrag(DragEvent(x1, y1));

        if (mask == 0x0)
        {
          if ((eventType != NV_MULTITOUCH_UP) && (eventType != NV_MULTITOUCH_CANCEL))
            LOG(LINFO, ("should be NV_MULTITOUCH_UP or NV_MULTITOUCH_CANCEL"));
        }

        if (m_mask == 0x2)
          m_work.StartDrag(DragEvent(x2, y2));

        if (mask == 0x3)
          m_work.StartScale(ScaleEvent(x1, y1, x2, y2));
      }

      if (m_mask == 0x2)
      {
        m_work.StopDrag(DragEvent(x2, y2));

        if (mask == 0x0)
        {
          if ((eventType != NV_MULTITOUCH_UP) && (eventType != NV_MULTITOUCH_CANCEL))
            LOG(LINFO, ("should be NV_MULTITOUCH_UP or NV_MULTITOUCH_CANCEL"));
        }

        if (mask == 0x1)
          m_work.StartDrag(DragEvent(x1, y1));

        if (mask == 0x3)
          m_work.StartScale(ScaleEvent(x1, y1, x2, y2));
      }

      if (m_mask == 0x3)
      {
        m_work.StopScale(ScaleEvent(m_x1, m_y1, m_x2, m_y2));

        if ((eventType == NV_MULTITOUCH_MOVE))
        {
          if (mask == 0x1)
            m_work.StartDrag(DragEvent(x1, y1));

          if (mask == 0x2)
            m_work.StartDrag(DragEvent(x2, y2));
        }
        else
          mask = 0;
      }
    }
    else
    {
      if (eventType == NV_MULTITOUCH_MOVE)
      {
        if (m_mask == 0x1)
          m_work.DoDrag(DragEvent(x1, y1));
        if (m_mask == 0x2)
          m_work.DoDrag(DragEvent(x2, y2));
        if (m_mask == 0x3)
          m_work.DoScale(ScaleEvent(x1, y1, x2, y2));
      }

      if ((eventType == NV_MULTITOUCH_CANCEL) || (eventType == NV_MULTITOUCH_UP))
      {
        if (m_mask == 0x1)
          m_work.StopDrag(DragEvent(x1, y1));
        if (m_mask == 0x2)
          m_work.StopDrag(DragEvent(x2, y2));
        if (m_mask == 0x3)
          m_work.StopScale(ScaleEvent(m_x1, m_y1, m_x2, m_y2));
        mask = 0;
      }
    }

    m_x1 = x1;
    m_y1 = y1;
    m_x2 = x2;
    m_y2 = y2;
    m_mask = mask;
    m_eventType = eventType;
  }

  void Framework::ShowSearchResult(search::Result const & r)
  {
    m_doLoadState = false;
    ::Framework::AddressInfo info;
    info.MakeFrom(r);
    ActivatePopupWithAddressInfo(r.GetFeatureCenter(), info);
    m_work.ShowSearchResult(r);
  }

  bool Framework::Search(search::SearchParams const & params)
  {
    return m_work.Search(params);
  }

  void Framework::LoadState()
  {
    if (!m_work.LoadState())
      m_work.ShowAll();
  }

  void Framework::SaveState()
  {
    m_work.SaveState();
  }

  void Framework::Invalidate()
  {
    m_work.Invalidate();
  }

  void Framework::SetupMeasurementSystem()
  {
    m_work.SetupMeasurementSystem();
  }

  void Framework::AddLocalMaps()
  {
    m_work.AddLocalMaps();
  }

  void Framework::RemoveLocalMaps()
  {
    m_work.RemoveLocalMaps();
  }

  void Framework::AddMap(string const & fileName)
  {
    m_work.AddMap(fileName);
  }

  void Framework::GetMapsWithoutSearch(vector<string> & out) const
  {
    ASSERT ( out.empty(), () );

    Platform const & pl = GetPlatform();

    vector<string> v;
    m_work.GetLocalMaps(v);

    for (size_t i = 0; i < v.size(); ++i)
    {
      // skip World and WorldCoast
      if (v[i].find(WORLD_FILE_NAME) == string::npos &&
          v[i].find(WORLD_COASTS_FILE_NAME) == string::npos)
      {
        try
        {
          FilesContainerR cont(pl.GetReader(v[i]));
          if (!cont.IsReaderExist(SEARCH_INDEX_FILE_TAG))
          {
            my::GetNameWithoutExt(v[i]);
            out.push_back(v[i]);
          }
        }
        catch (RootException const & ex)
        {
          // sdcard can contain dummy _*.mwm files. Supress this errors.
          LOG(LWARNING, ("Bad mwm file:", v[i], "Error:", ex.Msg()));
        }
      }
    }
  }

  storage::TIndex Framework::GetCountryIndex(double lat, double lon) const
  {
    return m_work.GetCountryIndex(m2::PointD(MercatorBounds::LonToX(lon),
                                             MercatorBounds::LatToY(lat)));
  }

  string Framework::GetCountryCode(double lat, double lon) const
  {
    return m_work.GetCountryCode(m2::PointD(MercatorBounds::LonToX(lon),
                                            MercatorBounds::LatToY(lat)));
  }

  string Framework::GetCountryNameIfAbsent(m2::PointD const & pt) const
  {
    using namespace storage;

    TIndex const idx = m_work.GetCountryIndex(pt);
    TStatus const status = m_work.GetCountryStatus(idx);
    if (status != EOnDisk && status != EOnDiskOutOfDate)
      return m_work.GetCountryName(idx);
    else
      return string();
  }

  m2::PointD Framework::GetViewportCenter() const
  {
    return m_work.GetViewportCenter();
  }

  void Framework::AddString(string const & name, string const & value)
  {
    m_work.AddString(name, value);
  }

  void Framework::Scale(double k)
  {
    m_work.Scale(k);
  }

  ::Framework * Framework::NativeFramework()
  {
    return &m_work;
  }

  void Framework::CallClickListener(double x, double y)
  {
    if (!HandleOnSmthClick(x, y))
      DeactivatePopup();
  }

  void Framework::CallLongClickListener(double x, double y)
  {
    if (!HandleOnSmthClick(x, y))
    {
      AdditionalHandlingForLongClick(x, y);
    }
  }


  bool Framework::HandleOnSmthClick(double x, double y)
  {
    BookmarkAndCategory bac = m_work.GetBookmark(m2::PointD(x, y));
    if (ValidateBookmarkAndCategory(bac))
    {
      Bookmark b = *(m_work.GetBmCategory(bac.first)->GetBookmark(bac.second));
      ActivatePopup(b.GetOrg(), b.GetName());
      return true;
    }
    else
    {
      ::Framework::AddressInfo adInfo;
      m2::PointD pxPivot;
      if (m_work.GetVisiblePOI(m2::PointD(x, y), pxPivot, adInfo))
      {
        ActivatePopupWithAddressInfo(m_work.PtoG(pxPivot), adInfo);
        return true;
      }
      else
        return false;
    }
  }

  bool Framework::AdditionalHandlingForLongClick(double x, double y)
  {
    m2::PointD point(x, y);
    ::Framework::AddressInfo adInfo;
    m_work.GetAddressInfo(point, adInfo);
    ActivatePopupWithAddressInfo(m_work.PtoG(point), adInfo);
  }

  /*
  void Framework::ToCamelCase(string & s)
  {
    if (s.length() > 0)
    {
      s[0] = toupper(s[0]);
      for(std::string::iterator it = s.begin() + 1; it != s.end(); ++it)
      {
          if(!isalpha(*(it - 1)) &&
             islower(*it))
          {
              *it = toupper(*it);
          }
      }
    }
  }
  */

  void Framework::ActivatePopupWithAddressInfo(m2::PointD const & bmkPosition, ::Framework::AddressInfo const & adInfo)
  {
    string name = adInfo.m_name;
    string type = "";
    if (adInfo.GetBestType() != 0)
      type = adInfo.GetBestType();
    string bmkname;
    if (name.empty() && type.empty())
    {
      bmkname = m_work.GetStringsBundle().GetString("dropped_pin");
    }
    else
      if (!name.empty())
      {
         bmkname = name;
      }
      else
        if (!type.empty())
        {
          bmkname = type;
        }
        else
        {
          std::stringstream cstream;
          cstream << name << " (" << type << ")";
          bmkname = cstream.str();
        }

    ActivatePopup(bmkPosition, bmkname);
    m_work.DrawPlacemark(bmkPosition);
    m_work.Invalidate();
  }

  void Framework::ActivatePopup(m2::PointD const & bmkPosition, string const & name)
  {
    gui::BookmarkBalloon * b = GetBookmarkBalloon();

    m_work.DisablePlacemark();
    b->setBookmarkPivot(bmkPosition);
    b->setBookmarkName(name);
    b->setIsVisible(true);
    m_work.Invalidate();
  }

  void Framework::DeactivatePopup()
  {
    gui::BookmarkBalloon * b = GetBookmarkBalloon();
    b->setIsVisible(false);
    m_work.DisablePlacemark();
    m_work.Invalidate();
  }

  void Framework::OnBalloonClick(gui::Element * e)
  {
    gui::BookmarkBalloon * balloon = GetBookmarkBalloon();

    BookmarkAndCategory bac = m_work.GetBookmark(m_work.GtoP(balloon->getBookmarkPivot()));
    if (ValidateBookmarkAndCategory(bac))
    {
      m_balloonClickListener(bac);
    }
    else
    {
      BookmarkCategory * cat;
      if (m_work.GetBmCategoriesCount() == 0)
      {
        m_work.AddBookmark(m_work.GetStringsBundle().GetString("my_places"), Bookmark(balloon->getBookmarkPivot(), balloon->getBookmarkName(), "placemark-red"));
        cat = m_work.GetBmCategory(m_work.GetBmCategoriesCount()-1);
      }
      else
      {
        cat = m_work.GetBmCategory(m_work.GetBmCategoriesCount()-1);
        LOG(LDEBUG,("Paladin", (balloon->getBookmarkPivot(), balloon->getBookmarkName(), "placemark-red")));
        m_work.AddBookmark(cat->GetName(), Bookmark(balloon->getBookmarkPivot(), balloon->getBookmarkName(), "placemark-red"));
      }
      cat->SaveToKMLFile();
      int catSize = cat->GetBookmarksCount() - 1;
      if (catSize > 0)
      {
        bac = BookmarkAndCategory(m_work.GetBmCategoriesCount()-1, catSize);
        m_balloonClickListener(bac);
      }
    }
  }

  void Framework::AddBalloonClickListener(TOnBalloonClickListener const & l)
  {
    m_balloonClickListener = l;
  }

  void Framework::RemoveBalloonClickListener()
  {
    m_balloonClickListener.clear();
  }

  void Framework::CreateBookmarkBalloon()
  {
    CHECK(m_work.GetGuiController(), ());
    CHECK(m_work.GetRenderPolicy(), ());

    gui::Balloon::Params bp;

    bp.m_position = graphics::EPosAbove;
    bp.m_depth = graphics::maxDepth;
    bp.m_pivot = m2::PointD(0, 0);
    bp.m_imageMarginBottom = 10;
    bp.m_imageMarginLeft = 10;
    bp.m_imageMarginRight = 10;
    bp.m_imageMarginTop = 10;
    bp.m_textMarginBottom = 10;
    bp.m_textMarginLeft = 10;
    bp.m_textMarginRight = 10;
    bp.m_textMarginTop = 10;
    bp.m_image = graphics::Image::Info("arrow.png", m_work.GetRenderPolicy()->Density());
    bp.m_text = "Bookmark";

    m_bmBaloon.reset(new gui::BookmarkBalloon(bp, &m_work));
    m_bmBaloon->setIsVisible(false);
    m_bmBaloon->setOnClickListener(bind(&Framework::OnBalloonClick, this, _1));
    m_work.GetGuiController()->AddElement(m_bmBaloon);
  }
}
