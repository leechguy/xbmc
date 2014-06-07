/*
 *      Copyright (C) 2005-2014 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "system.h"
#include "GUIVideoBackground.h"
#include "GUIWindowManager.h"
#include "GUIUserMessages.h"
#include "Application.h"
#ifdef HAS_VIDEO_PLAYBACK
#include "cores/VideoRenderers/RenderManager.h"
#else
#include "cores/DummyVideoPlayer.h"
#endif
#include "utils/log.h"

#include "guilib/GraphicContext.h"
#include "guilib/Texture.h"
#include "guilib/GUITexture.h"
#include "filesystem/SpecialProtocol.h"
#include "video/FFmpegVideoDecoder.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
}

SimpleVideoPlayer::SimpleVideoPlayer()
{
  m_decoder = new FFmpegVideoDecoder();
  m_timeFromPrevSong = 0.0;
  m_texture = 0;

  m_bIsPlaying = false;
  m_bHasVideo = true;
}

SimpleVideoPlayer::~SimpleVideoPlayer()
{
  delete m_decoder;
  delete m_texture;
}

bool SimpleVideoPlayer::OpenFile(const CFileItem& file, const CPlayerOptions& options)
{
  return Start(file.GetPath());
}

bool SimpleVideoPlayer::openVideoFile( const CStdString& filename )
{
  CStdString realPath = CSpecialProtocol::TranslatePath( filename );

  if ( !m_decoder->open( realPath ) )
  {
    CLog::Log( LOGERROR, "SimpleVideoPlayer: %s, video file %s (%s)", m_decoder->getErrorMsg().c_str(), filename.c_str(), realPath.c_str() );
    return false;
  }

  m_videoWidth = m_decoder->getWidth();
  m_videoHeight = m_decoder->getHeight();
  m_curVideoFile = filename;

  // Find out the necessary aspect ratio for height (assuming fit by width) and width (assuming fit by height)
  const RESOLUTION_INFO info = g_graphicsContext.GetResInfo();
  m_displayLeft   = info.Overscan.left;
  m_displayRight  = info.Overscan.right;
  m_displayTop    = info.Overscan.top;
  m_displayBottom = info.Overscan.bottom;

  int screen_width = m_displayRight - m_displayLeft;
  int screen_height = m_displayBottom - m_displayTop;

  // Do we need to modify the output video size? This could happen in two cases:
  // 1. Either video dimension is larger than the screen - video needs to be downscaled
  // 2. Both video dimensions are smaller than the screen - video needs to be upscaled
  if ( ( m_videoWidth > 0 && m_videoHeight > 0 )
  && ( ( m_videoWidth > screen_width || m_videoHeight > screen_height )
  || ( m_videoWidth < screen_width && m_videoHeight < screen_height ) ) )
  {
    // Calculate the scale coefficients for width/height separately
    double scale_width = (double) screen_width / (double) m_videoWidth;
    double scale_height = (double) screen_height / (double) m_videoHeight;

    // And apply the smallest
    double scale = scale_width < scale_height ? scale_width : scale_height;
    m_videoWidth = (int) (m_videoWidth * scale);
    m_videoHeight = (int) (m_videoHeight * scale);
  }

  // Calculate the desktop dimensions to show the video
  if ( m_videoWidth < screen_width || m_videoHeight < screen_height )
  {
    m_displayLeft = (screen_width - m_videoWidth) / 2;
    m_displayRight -= m_displayLeft;

    m_displayTop = (screen_height - m_videoHeight) / 2;
    m_displayBottom -= m_displayTop;
  }

  m_millisecondsPerFrame = 1.0 / m_decoder->getFramesPerSecond();

  CLog::Log( LOGDEBUG, "SimpleVideoPlayer: Video file %s (%dx%d) length %g seconds opened successfully, will be shown as %dx%d at (%d, %d - %d, %d) rectangle",
             filename.c_str(),
             m_decoder->getWidth(), m_decoder->getHeight(),
             m_decoder->getDuration(),
             m_videoWidth, m_videoHeight,
             m_displayLeft, m_displayTop, m_displayRight, m_displayBottom );

  return true;
}

void SimpleVideoPlayer::CloseFile()
{
  Stop();
  m_decoder->close();
}

void SimpleVideoPlayer::Render(int left, int top, int right, int bottom)
{
  // Just in case
  if ( !m_texture )
    return;


  // Get the current time in ms.
  double current = XbmcThreads::SystemClockMillis();

  // We're supposed to show m_decoder->getFramesPerSecond() frames in one second.
  if ( current >= m_nextFrameTime )
  {
	// We don't care to adjust for the exact timing as we don't worry about the exact frame rate
	m_nextFrameTime = current + m_millisecondsPerFrame - (current - m_nextFrameTime);

	while ( true )
	{
	  if ( !m_decoder->nextFrame( m_texture ) )
	  {
		// End of video; restart
		m_decoder->seek( 0.0 );
		m_nextFrameTime = 0.0;
		continue;
	  }

	  break;
	}
  }

  CLog::Log(LOGDEBUG, "SimpleVideoPlayer::Render() 1: %d, %d, %d, %d", left, top, right, bottom);
  CLog::Log(LOGDEBUG, "SimpleVideoPlayer::Render() 2: %d, %d, %d, %d", m_displayLeft, m_displayTop, m_displayRight, m_displayBottom);


  // We got a frame. Draw it.
  CRect vertCoords((float) m_displayLeft, (float) m_displayTop, (float) m_displayRight, (float) m_displayBottom );
//  CRect vertCoords((float) left, (float) top, (float) right, (float) bottom );
  CGUITexture::DrawQuad(vertCoords, 0xffffffff, m_texture );

}

bool SimpleVideoPlayer::Start( const CStdString& filename )
{
  if ( !filename.empty() )
  {
     if ( !openVideoFile( filename ) )
       return false;

     m_timeFromPrevSong = 0;
  }
//  else
//  {
//     if ( !openVideoFile( g_advancedSettings.m_karaokeDefaultBackgroundFilePath ) )
//       return false;
//
//	 if ( m_timeFromPrevSong != 0.0 && !m_decoder->seek( m_timeFromPrevSong ) )
//	   m_timeFromPrevSong = 0;
//  }

  // Allocate the texture
  m_texture = new CTexture( m_videoWidth, m_videoHeight, XB_FMT_A8R8G8B8 );

  if ( !m_texture )
  {
    CLog::Log( LOGERROR, "SimpleVideoPlayer: Could not allocate texture" );
    return false;
  }

  m_bIsPlaying = true;
  m_nextFrameTime = 0.0;
  return true;
}

void SimpleVideoPlayer::Stop()
{
  delete m_texture;
  m_texture = 0;

  m_timeFromPrevSong = m_decoder->getLastFrameTime();

  m_bIsPlaying = false;
}


CGUIVideoBackground::CGUIVideoBackground(int parentID, int controlID, float posX, float posY, float width, float height)
    : CGUIControl(parentID, controlID, posX, posY, width, height)
{
  ControlType = GUICONTROL_VIDEOBACKGROUND;

//  m_strFilename = strFilename;
//  CFileItem item(m_strFilename, false);
//  m_item = item;
//
  m_eCurrentPlayer = EPC_NONE;
  m_pPlayer = NULL;

  m_options.video_only = true;
  m_options.fullscreen = false;
  m_options.starttime = 0.0f;
//  m_options.loop = true;

  // assign a unique id to the CGUIVideoBackground instance
  static int id = 0;
  m_videoBackgroundId = id++;
}

CGUIVideoBackground::~CGUIVideoBackground(void)
{
  CLog::Log(LOGDEBUG, "CGUIVideoBackground (id=%d): Destructor called", m_videoBackgroundId);
  if (m_pPlayer)
  {
	m_pPlayer->CloseFile();
	delete m_pPlayer;
  }
}

void CGUIVideoBackground::Process(unsigned int currentTime, CDirtyRegionList &dirtyregions)
{
//  CLog::Log(LOGDEBUG, "CGUIVideoBackground::Process() (id=%d): g_application.IsPlayingVideo() = %d", m_videoBackgroundId, g_application.IsPlayingVideo());
  if (!IsPlayingVideo())
    AllocResources();
//  else if (g_renderManager.RendererHandlesPresent())
//    MarkDirtyRegion();

  MarkDirtyRegion();

  CGUIControl::Process(currentTime, dirtyregions);
}

void CGUIVideoBackground::Render()
{
  if (!IsVisible()) return;
//
//#ifdef HAS_VIDEO_PLAYBACK
//  // don't render if we are playing video, or if the renderer isn't started
//  // (otherwise the lock we have from CApplication::Render() may clash with the startup
//  // locks in the RenderManager.)
//  if (/*!g_application.IsPlayingVideo() && */g_renderManager.IsStarted() && IsPlayingVideo())
//  {
//#else
//  if (!g_application.IsPlayingVideo() && IsPlayingVideo())
//  {
//#endif
//
//
//  g_graphicsContext.SetViewWindow(m_posX, m_posY, m_posX + m_width, m_posY + m_height);
////  CLog::Log(LOGDEBUG, "CGUIVideoBackground::Render() (id=%d): m_posX, m_posY, m_posX + m_width, m_posY + m_height) = %.1f, %.1f, %.1f, %.1f", m_videoBackgroundId, m_posX, m_posY, m_posX + m_width, m_posY + m_height);
////  CRect source, dest;
////  g_renderManager.GetVideoRect(source, dest);
////  CLog::Log(LOGDEBUG, "CGUIVideoBackground::Render() (id=%d): source(x1, y1, x2, y2) = %.1f, %.1f, %.1f, %.1f", m_videoBackgroundId, source.x1, source.y1, source.x2, source.y2);
////  CLog::Log(LOGDEBUG, "CGUIVideoBackground::Render() (id=%d):   dest(x1, y1, x2, y2) = %.1f, %.1f, %.1f, %.1f", m_videoBackgroundId, dest.x1, dest.y1, dest.x2, dest.y2);
//
////  if (g_application.IsPresentFrame())
////	  g_renderManager.Present();
////  else{
////	  //color_t alpha = g_graphicsContext.MergeAlpha(0xFF000000) >> 24;
////	  g_renderManager.RenderUpdate(true, 0, 255);
////  }
//
//
//#ifdef HAS_VIDEO_PLAYBACK
//    color_t alpha = g_graphicsContext.MergeAlpha(0xFF000000) >> 24;
//    g_renderManager.Render(false, 0, alpha);
//#else
//    ((CDummyVideoPlayer *)g_application.m_pPlayer->GetInternal())->Render();
//#endif
//  }

  m_pPlayer->Render(m_posX, m_posY, m_posX + m_width, m_posY + m_height);

  CGUIControl::Render();
}

void CGUIVideoBackground::UpdateVisibility(const CGUIListItem *item /* = NULL */)
{
  CGUIControl::UpdateVisibility(item);
  if (!IsVisible()) FreeResources();
}

void CGUIVideoBackground::AllocResources()
{

  if (m_strFileName.empty() || m_item.m_bIsFolder || m_strFileName.Equals(m_item.GetPath()))
  {
	CLog::Log(LOGDEBUG, "CGUIVideoBackground::AllocResources (id=%d) aborting", m_videoBackgroundId);
    return;
  }
  CGUIControl::AllocResources();

  CLog::Log(LOGDEBUG, "CGUIVideoBackground::AllocResources (id=%d)", m_videoBackgroundId);
  if (!m_pPlayer || (m_pPlayer && !IsPlayingVideo()))
  {
	m_item = CFileItem(m_strFileName, false);

    // Sleep to prevent vdpau from crashing
//    Sleep(3000);
    PlayFile();
  }
}

void CGUIVideoBackground::FreeResources(bool immediately /* = false */)
{
  CLog::Log(LOGDEBUG, "CGUIVideoBackground::FreeResources (id=%d): immediately = %d", m_videoBackgroundId, immediately);
  if (m_pPlayer)
  {
    m_pPlayer->CloseFile();
    m_item.Reset();
    if (!m_info.IsConstant())
    	m_strFileName.clear();
  }
  CGUIControl::FreeResources(immediately);
}

bool CGUIVideoBackground::OnMessage(CGUIMessage& message)
{
//	switch (message.GetMessage())
//	{
//	case GUI_MSG_PLAYBACK_STARTED:
//		CLog::Log(LOGINFO, "CGUIVideoBackground::%s (id=%d): Received message: GUI_MSG_PLAYBACK_STARTED", __FUNCTION__, m_videoBackgroundId);
//		break;
//	case GUI_MSG_PLAYBACK_ENDED:
//		CLog::Log(LOGINFO, "CGUIVideoBackground::%s (id=%d): Received message: GUI_MSG_PLAYBACK_ENDED", __FUNCTION__, m_videoBackgroundId);
//		// when we're not in fullscreen mode, we need to fire up the background video when the main player stops playback
//		PlayFile();
//		break;
//	case GUI_MSG_PLAYBACK_STOPPED:
//		CLog::Log(LOGINFO, "CGUIVideoBackground::%s (id=%d): Received message: GUI_MSG_PLAYBACK_STOPPED", __FUNCTION__, m_videoBackgroundId);
//		// when we're not in fullscreen mode, we need to fire up the background video when the main player stops playback
//		PlayFile();
//	default:
//		break;
//	}

  return CGUIControl::OnMessage(message);
}

bool CGUIVideoBackground::CanFocus() const
{ // unfocusable
  return false;
}

void CGUIVideoBackground::UpdateInfo(const CGUIListItem *item /* = NULL */)
{
  if (m_info.IsConstant())
    return; // nothing to do

  if (item)
  {
    CLog::Log(LOGDEBUG, "CGUIVideoBackground::UpdateInfo() (id=%d): item not NULL", m_videoBackgroundId);
    SetFileName(m_info.GetItemLabel(item, true, &m_currentFallback));
  }
  else
  {
    CLog::Log(LOGDEBUG, "CGUIVideoBackground::UpdateInfo() (id=%d): item is NULL", m_videoBackgroundId);
    SetFileName(m_info.GetLabel(m_parentID, true, &m_currentFallback));
  }
}

void CGUIVideoBackground::SetInfo(const CGUIInfoLabel &info)
{
	CLog::Log(LOGDEBUG, "CGUIVideoBackground::SetInfo() (id=%d): info", m_videoBackgroundId);
	m_info = info;
	// a constant video never needs updating
	if (m_info.IsConstant())
	{
		m_strFileName = m_info.GetLabel(0);
		CLog::Log(LOGDEBUG, "CGUIVideoBackground::SetInfo() (id=%d): m_info.IsConstant() = true, m_strFileName = %s", m_videoBackgroundId, m_strFileName.c_str());
	}
}

void CGUIVideoBackground::SetFileName(const CStdString& strFileName, bool setConstant /* = false */)
{
  CLog::Log(LOGDEBUG, "CGUIVideoBackground::SetFileName() (id=%d): filename = %s", m_videoBackgroundId, strFileName.c_str());
  if (m_strFileName.Equals(strFileName)) return;

  if (setConstant)
    m_info.SetLabel(strFileName, "", GetParentID());

  FreeResources(false);
  m_strFileName = strFileName;
}

void CGUIVideoBackground::SetLoop(bool loop /* = true */)
{
//  m_options.loop = loop;
}

bool CGUIVideoBackground::IsPlayingVideo() const
{
  return (m_pPlayer && m_pPlayer->IsPlaying() && m_pPlayer->HasVideo());
}

void CGUIVideoBackground::PlayFile()
{
  CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): Starting", m_videoBackgroundId);

  if (!m_pPlayer)
  {
    CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): Create new Player", m_videoBackgroundId);
    m_pPlayer = new SimpleVideoPlayer();
  }

  if (m_pPlayer) // && m_videoBackgroundId > 0)
  {
    if (!m_pPlayer->OpenFile(m_item, m_options))
    {
      CLog::Log(LOGERROR, "CGUIVideoBackground::PlayFile() (id=%d): Error creating player for item %s (File doesn't exist?)", m_videoBackgroundId, m_item.GetPath().c_str());
      //TODO: Check if this is the fallback video, if not, try to load the fallback video
    }
  }
  else
  {
    CLog::Log(LOGERROR, "CGUIVideoBackground::PlayFile() (id=%d): failed to create player", m_videoBackgroundId);
  }

}

//void CGUIVideoBackground::PlayFile()
//{
//  CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): Starting", m_videoBackgroundId);
//
//  // Don't play if the main player is already playing since this
//  // will give a horrible flicker when both try to paint on the
//  // background when in non-fullscreen mode
////  if (g_application.IsPlayingVideo())
////  {
////    CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): g_application.IsPlayingVideo() = true", m_videoBackgroundId);
////    return;
////  }
//
//
//  PLAYERCOREID eNewCore = CPlayerCoreFactory::Get().GetDefaultPlayer(m_item);
//
//  // We should restart the player, unless the previous and next tracks are using
//  // one of the players that allows gapless playback (dvdplayer, omxplayer)
//  if (m_pPlayer)
//  {
//    if ( !(m_eCurrentPlayer == eNewCore && (m_eCurrentPlayer == EPC_DVDPLAYER
//#if defined(HAS_OMXPLAYER)
//            || m_eCurrentPlayer == EPC_OMXPLAYER
//#endif
//            )) )
//    {
//      CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): Delete existing player", m_videoBackgroundId);
//      delete m_pPlayer;
//      m_pPlayer = NULL;
//    }
//  }
//
//  if (!m_pPlayer)
//  {
//    CLog::Log(LOGDEBUG, "CGUIVideoBackground::PlayFile() (id=%d): Create new Player", m_videoBackgroundId);
//    m_pPlayer = CPlayerCoreFactory::Get().CreatePlayer(eNewCore, *this);
//  }
//
//  // TODO: figure out why the first player deadlocks XBMC during startup
//  if (m_pPlayer && m_videoBackgroundId > 0)
//  {
//    m_eCurrentPlayer = eNewCore;
//    CSingleExit ex(g_graphicsContext);
//    if (!m_pPlayer->OpenFile(m_item, m_options))
//    {
//        CLog::Log(LOGERROR, "CGUIVideoBackground::PlayFile() (id=%d): Error creating player for item %s (File doesn't exist?)", m_videoBackgroundId, m_item.GetPath().c_str());
//        //TODO: Check if this is the fallback video, if not, try to load the fallback video
//    }
//  }
//  else
//  {
//	  CLog::Log(LOGERROR, "CGUIVideoBackground::PlayFile() (id=%d): failed to create player", m_videoBackgroundId);
//  }
//
//}

