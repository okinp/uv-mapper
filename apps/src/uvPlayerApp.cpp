/*
 Copyright (c) Aldo Hoeben / fieldOfView
 All rights reserved.

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that
 the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and
	the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
	the following disclaimer in the documentation and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.
*/

#include "cinder/app/AppBasic.h"
#include "cinder/Utilities.h"
#include "cinder/Text.h"

#include "cinder/ImageIo.h"
#include "cinder/Surface.h"

#include "cinder/gl/gl.h"
#include "cinder/gl/texture.h"
#include "cinder/gl/GlslProg.h"
#include "cinder/gl/Fbo.h"

#include "cinder/qtime/QuickTime.h"
#include "cinder/qtime/MovieWriter.h"

#include "uvPlayerResources.h"

#include <vector>

using namespace ci;
using namespace ci::app;
using namespace std;

#define APP_WIDTH 700
#define APP_HEIGHT 420

class uvPlayerApp : public AppBasic {
public:
	void prepareSettings ( Settings *settings );
	void setup ();
	void update ();
	void draw ();
	
	void resize ( ResizeEvent event );
	void keyDown ( KeyEvent event );
	void fileDrop( FileDropEvent event );	
	
	string argument ( string argumentName, string defaultValue );	

	enum stateType {
		STATE_PLAYING,
		STATE_EXPORTING,
		STATE_PATTERNS
	};
	stateType mState;

	// player
	void loadMovieFile( const fs::path &path );
	void loadMapFile  ( fs::path &path );
	void loadOverlayFile ( const fs::path &path );
	void defaultMap   ();
	void defaultImage ();
	void splitMap ( Surface16u sourceMap, gl::Texture *msbTexture, gl::Texture *lsbTexture );
	void infoTexture  ( const string &title );

	bool			mUse8bitPath;

	bool			mShowInfo;
	gl::Texture		mFrameTexture, mInfoTexture, mOverlayTexture;
	vector<gl::Texture>	mMapTexture, mMapMSBTexture, mMapLSBTexture;
	qtime::MovieGl	mMovie;
	gl::GlslProg	mShader;

	// exporting
	fs::path			mExportPath;
	qtime::MovieWriter	mMovieWriter;
	gl::Fbo				mRenderBuffer;

	// patterns
	Channel graycodes ( int aBits );

	Channel mGraycode;
	gl::Texture mPatternTexture;

	int mGraycodeLine;
	bool mPatternAxis;
};

void uvPlayerApp::prepareSettings( Settings *settings )
{
	settings->setWindowSize(APP_WIDTH, APP_HEIGHT);
	settings->setTitle("UV Mapper Player");
}

void uvPlayerApp::setup()
{
	// TODO: properly determine if 8bit path is necessary; this checks GL_RGBA16F, we need GL_RGBA16
	mUse8bitPath = !( gl::isExtensionAvailable( "GL_ARB_half_float_pixel" ) );

	// load shader
	try {
		if( !mUse8bitPath )
			mShader = gl::GlslProg( loadResource( RES_PASSTHRU_VERT ), loadResource( RES_UVMAP_FRAG ) );
		else
			mShader = gl::GlslProg( loadResource( RES_PASSTHRU_VERT ), loadResource( RES_UVMAP_8BIT_FRAG ) );
	}
	catch( gl::GlslProgCompileExc &exc ) {
		console() << "Shader compile error: " << endl;
		console() << exc.what();
		AppBasic::quit();
		return;
	}
	catch( ... ) {
		console() << "Unable to load shader" << endl;
		AppBasic::quit();
		return;
	}
	
	// load default map and image
	fs::path mapPath = argument("map","");
	if( ! mapPath.empty() ) 
		loadMapFile( mapPath );
	else
		defaultMap();
	
	fs::path moviePath = argument("file","");
	if( ! moviePath.empty() )
		loadMovieFile( moviePath );
	else
		defaultImage();

	mOverlayTexture.reset();
    mShowInfo = true;
	mState = STATE_PLAYING;
}

void uvPlayerApp::resize( ResizeEvent event )
{
	int size = max( getWindowWidth(), getWindowHeight() );
	int bits = (int)ceil( log( (double)size ) / log( (double)2 ) );
	mGraycode = graycodes( bits );

	mPatternTexture = gl::Texture( mGraycode );
	mPatternTexture.setMagFilter( GL_NEAREST );

	mGraycodeLine = 0;
	mPatternAxis = false;
}

void uvPlayerApp::keyDown( KeyEvent event )
{
	fs::path path;

	switch ( event.getCode() ) {
	case KeyEvent::KEY_ESCAPE:
		if(mState == STATE_EXPORTING) {
			mState = STATE_PLAYING;
			if(mMovie) {
				mMovieWriter.finish();
				mMovie.seekToStart();
				mMovie.play();
			}

		} else {
			if(isFullScreen()) {
				showCursor();
				setFullScreen( false );
			} else
				AppBasic::quit();
		}			
		break;
		
	case KeyEvent::KEY_f:
		if( mState == STATE_EXPORTING )
			break;

		if( !isFullScreen() ) {
			hideCursor();
			mShowInfo = false;
		} else
			showCursor();

		setFullScreen( !isFullScreen() );

		break;

	case KeyEvent::KEY_i:
		mShowInfo = !mShowInfo;
			
		break;

	case KeyEvent::KEY_SPACE:
		if(mState == STATE_PLAYING && mMovie) {
			if(mMovie.isPlaying())
				mMovie.stop();
			else
				mMovie.play();
		}
		break;

	case KeyEvent::KEY_BACKSPACE:
		if(mState == STATE_PLAYING && mMovie) 
			mMovie.seekToStart();
		
		break;

	case KeyEvent::KEY_o:
		// _Open/reset movie/image
		if( !event.isShiftDown() ) {
			showCursor();
			setFullScreen(false);
			path = getOpenFilePath();
			if( !path.empty() ) 
				loadMovieFile( path );
		} else {
			mMovie.reset();
			defaultImage();
		}
		break;

	case KeyEvent::KEY_m:
		// Open/reset _map
		if( !event.isShiftDown() ) {
			showCursor();
			setFullScreen(false);
			path = getOpenFilePath();
			if( !path.empty() ) 
				loadMapFile( path );
		} else
			defaultMap();
		
		break;

	case KeyEvent::KEY_l:
		// Open/reset over_lay
		if( !event.isShiftDown() ) {
			showCursor();
			setFullScreen(false);
			path = getOpenFilePath();
			if( !path.empty() ) 
				loadOverlayFile( path );
		} else 
			mOverlayTexture.reset();
		
		break;

	case KeyEvent::KEY_e:
		// Export movie
		if(!mMovie)
			break;

		switch( mState ) {
		case STATE_PLAYING:
			showCursor();
			setFullScreen(false);

			mExportPath = getSaveFilePath();
			if( mExportPath.empty() ) 
				break;

			// make sure we're either creating TIF for stills or MOV for movie output
			mExportPath.replace_extension( ( mMovie.getNumFrames() > 1 ) ? ".mov" : ".tif" ); 

			if( mMovie.getNumFrames() > 1 ) {
				qtime::MovieWriter::Format qtFormat;
				if( !(qtime::MovieWriter::getUserCompressionSettings( &qtFormat, loadImage( loadResource( RES_DEFAULT_IMAGE ) ) ) ) )
					break;

				mMovieWriter = qtime::MovieWriter( mExportPath, mRenderBuffer.getWidth(), mRenderBuffer.getHeight(), qtFormat );

				mMovie.stop();
				mMovie.seekToStart();
			}
			mState = STATE_EXPORTING;

			break;

		case STATE_EXPORTING:
			mState = STATE_PLAYING;

			mMovieWriter.finish();
			mMovie.seekToStart();
			mMovie.play();
			break;

		case STATE_PATTERNS:
			// nothing
			break;
		}

		break;

	case KeyEvent::KEY_p:
		switch( mState ) {
		case STATE_PLAYING:
			if(mMovie) 
				mMovie.stop();			

			mState = STATE_PATTERNS;

			mGraycodeLine = 0;
			mPatternAxis = false;

			break;
		case STATE_PATTERNS:
			mState = STATE_PLAYING;

			if(mMovie) { 
				mMovie.seekToStart();
				mMovie.play();
			}

			break;

		case STATE_EXPORTING:
			// nothing
			break;
		}
		
		break;

	case KeyEvent::KEY_LEFT:
		if( mState == STATE_PATTERNS ) {
		
			if ( mGraycodeLine > 0 )
				mGraycodeLine--;
			else {
				mGraycodeLine = mGraycode.getHeight() - 1;
				mPatternAxis = !mPatternAxis;
			}
		}
		break;

	case KeyEvent::KEY_RIGHT:
		if( mState == STATE_PATTERNS ) {

			if ( mGraycodeLine < ( mGraycode.getHeight() - 1 ) ) 
				mGraycodeLine++;
			else {
				mGraycodeLine = 0;
				mPatternAxis = !mPatternAxis;
			}
		}

		break;

	}
}

void uvPlayerApp::fileDrop( FileDropEvent event )
{
	if( mState == STATE_PLAYING )
		loadMovieFile( event.getFile( 0 ) );
}

void uvPlayerApp::update()
{
	switch( mState ) {
	case STATE_PATTERNS:
		//nothing;
		break;
	case STATE_PLAYING:
	case STATE_EXPORTING:
		if( mState == STATE_PLAYING ) {
			if( mMovie && mMovie.checkNewFrame() ) {
				mFrameTexture = mMovie.getTexture();
			}
		} else {
			if( mMovie.getNumFrames() > 1 ) {
				if( !( mMovie.getCurrentTime() >= mMovie.getDuration() - ( 1 / mMovie.getFramerate() ) ) ) {
					mMovie.stepForward();
					mFrameTexture = mMovie.getTexture();
				} else {
					mMovie.seekToStart();
					mMovie.play();

					mMovieWriter.finish();
					mFrameTexture.reset();

					mState = STATE_PLAYING;
				}
			}
		}

		// bind the FBO, so we can draw on it
		mRenderBuffer.bindFramebuffer();
	
		// set the correct viewport and matrices
		glPushAttrib( GL_VIEWPORT_BIT );
		gl::setViewport( mRenderBuffer.getBounds() );
	
		gl::pushMatrices();
		gl::setMatricesWindow( mRenderBuffer.getSize() );

		// clear the buffer
		gl::clear( Color( 0, 0, 0 ) );

		// draw Fbo upsidedown, because.
		Rectf bufferRect = Rectf( 0, (float)mRenderBuffer.getHeight(), (float)mRenderBuffer.getWidth(), 0 );

		if( mFrameTexture ) {
			// use uvmap shader to draw frame into Fbo
			mShader.bind();

			mShader.uniform( "frameSize", Vec2f( (float)mFrameTexture.getWidth(), (float)mFrameTexture.getHeight() ) );
			mShader.uniform( "flipv", mFrameTexture.isFlipped() );

			for( vector<gl::Texture>::size_type pass = 0; pass != mMapTexture.size(); pass++) {
				if( !mUse8bitPath ) {
					mMapTexture[ pass ].bind( 0 );
					mShader.uniform( "map", 0 );

					mFrameTexture.bind( 1 );
					mShader.uniform( "frame", 1 );
				} else {
					mMapMSBTexture[ pass ].bind( 0 );
					mShader.uniform( "mapMSB", 0 );

					mMapLSBTexture[ pass ].bind( 1 );
					mShader.uniform( "mapLSB", 1 );

					mFrameTexture.bind( 2 );
					mShader.uniform( "frame", 2 );				
				}
		
				gl::drawSolidRect( bufferRect );
		
				if( !mUse8bitPath ) {
					mMapTexture[ pass ].unbind();
				} else {
					mMapMSBTexture[ pass ].unbind();
					mMapLSBTexture[ pass ].unbind();
				}
			}
			mFrameTexture.unbind();
			mShader.unbind();
		}

		if( mOverlayTexture ) {
			gl::draw( mOverlayTexture, bufferRect );
		}

		// restore matrices and viewport
		gl::popMatrices();
		glPopAttrib();

		// unbind the FBO to stop drawing on it
		mRenderBuffer.unbindFramebuffer();

		break;
	}
}

void uvPlayerApp::draw()
{
	gl::enableAlphaBlending();
	gl::clear( Color( 0, 0, 0 ) );
	
	switch( mState ) {
	case STATE_PLAYING:
	case STATE_EXPORTING: 
		// draw renderbuffer to screen
		gl::draw( mRenderBuffer.getTexture(), Rectf( mRenderBuffer.getBounds() ).getCenteredFit( getWindowBounds(), true ) );

		if( mState == STATE_EXPORTING ) {
			if( mMovie.getNumFrames() > 1 ) {
				mMovieWriter.addFrame( Surface(mRenderBuffer.getTexture() ) );
			} else {
				writeImage( mExportPath, Surface(mRenderBuffer.getTexture() ) );	
				mState = STATE_PLAYING; 
			}
		}

		if( mInfoTexture && mShowInfo ) {
			glDisable( GL_TEXTURE_RECTANGLE_ARB );
			gl::draw( mInfoTexture, Vec2f( 20, getWindowHeight() - 20 - (float)mInfoTexture.getHeight() ) );
		}

		break;

	case STATE_PATTERNS:

		int32_t patternWidth = mGraycode.getWidth();
		int32_t offset = 0;

		if(mPatternTexture) {
			if( ! mPatternAxis ) {
				offset = ( patternWidth - getWindowWidth() ) / 2;

				gl::translate( Vec2f( (float)-offset, 0 ) );
			
				gl::draw( mPatternTexture, Area( 0, mGraycodeLine, mGraycode.getWidth(), mGraycodeLine+1 ), Area( 0, 0, patternWidth, patternWidth ) ); 
			
				gl::translate( Vec2f( (float)offset, 0 ) );
			} else {
				offset = ( patternWidth - getWindowHeight() ) / 2;

				gl::translate( Vec2f( (float)patternWidth , (float)-offset ) );
				gl::rotate( 90.0 );

				gl::draw( mPatternTexture, Area( 0, mGraycodeLine, mGraycode.getWidth(), mGraycodeLine+1 ), Area( 0, 0, patternWidth, patternWidth ) ); 

				gl::rotate( -90.0 );
				gl::translate( Vec2f( -(float)patternWidth , (float)offset ) );
			}
		}
		if( mOverlayTexture ) {
			gl::draw( mOverlayTexture, getWindowBounds() );
		}

		break;
	}
}

string uvPlayerApp::argument(string argumentName, string defaultValue = "")
{
	// utility method: find argument from commandline arguments
	for( vector<string>::const_iterator argIter = getArgs().begin(); argIter != getArgs().end(); ++argIter ) {
		if(("-"+argumentName) == *argIter && argIter != getArgs().end()) {
			++argIter;
			return *argIter;
		}
	}
	return defaultValue;
}

void uvPlayerApp::loadMovieFile( const fs::path &moviePath )
{
	try {
		// load up the movie, set it to loop, and begin playing
		mMovie = qtime::MovieGl( moviePath );
		mMovie.setLoop();
		mMovie.play();

		infoTexture( moviePath.filename().string() );
	}
	catch( ... ) {
		console() << "Unable to load the movie." << endl;
		mMovie.reset();
		defaultImage();
	}
	
	mFrameTexture.reset();
}

void uvPlayerApp::defaultImage()
{
	gl::Texture::Format format;
	format.setTargetRect();

	mFrameTexture = gl::Texture( loadImage( loadResource( RES_DEFAULT_IMAGE ) ), format );

	infoTexture( "No movie loaded" );
}

void uvPlayerApp::loadMapFile( fs::path &mapPath )
{
	mMapTexture.clear();
	mMapMSBTexture.clear();
	mMapLSBTexture.clear();
	
	Surface16u mapImage;
	while ( fs::exists( mapPath ) ) {
		try {
			mapImage = loadImage( mapPath );
		}
		catch( ... ) {
			console() << "Unable to load uv map file." << endl;
		
			defaultMap();
			return;
		};

		mMapTexture.push_back( gl::Texture( mapImage ) );
		if( mUse8bitPath ) {
			mMapMSBTexture.push_back( gl::Texture() );
			mMapLSBTexture.push_back( gl::Texture() );
			splitMap( mapImage, &mMapMSBTexture[ mMapMSBTexture.size()-1 ], &mMapLSBTexture[ mMapLSBTexture.size()-1 ] );
		}

		string newPath = mapPath.string();
		newPath[ newPath.length() - mapPath.extension().string().length() - 1 ]++;
		mapPath = fs::path( newPath );
	}
	mRenderBuffer = gl::Fbo( mapImage.getWidth(), mapImage.getHeight(), false );    
}

void uvPlayerApp::defaultMap()
{
	Surface16u defaultMap = Surface16u( APP_WIDTH, APP_HEIGHT, false, SurfaceChannelOrder::RGB );
	Surface16u::Iter defaultMapIter( defaultMap.getIter() );
	
	while ( defaultMapIter.line() ) {
		uint16_t vValue = (uint16_t)(( defaultMapIter.y() << 16 ) / APP_HEIGHT );
		while ( defaultMapIter.pixel() ) {
			defaultMapIter.r() = (uint16_t)(( defaultMapIter.x() << 16 ) / APP_WIDTH );
			defaultMapIter.g() = vValue;
		}
	}

	if( mUse8bitPath ) {
		mMapMSBTexture.clear();
		mMapMSBTexture.push_back( gl::Texture() );
		mMapLSBTexture.clear();
		mMapLSBTexture.push_back( gl::Texture() );
		splitMap( defaultMap, &mMapMSBTexture[0], &mMapLSBTexture[0] );
	}
	mMapTexture.clear();
	mMapTexture.push_back( gl::Texture( defaultMap ) );


	mRenderBuffer = gl::Fbo( defaultMap.getWidth(), defaultMap.getHeight(), false );
}

void uvPlayerApp::splitMap( Surface16u sourceMap, gl::Texture *msbTexture, gl::Texture *lsbTexture ) 
{
	Surface16u::Iter sourceMapIter( sourceMap.getIter() );
	Surface8u mapMSB( sourceMap.getWidth(), sourceMap.getHeight(), sourceMap.hasAlpha() );
	Surface8u::Iter mapMSBIter( mapMSB.getIter() );
	Surface8u mapLSB( sourceMap.getWidth(), sourceMap.getHeight(), sourceMap.hasAlpha() );
	Surface8u::Iter mapLSBIter( mapLSB.getIter() );
	
	uint16_t uValue, vValue, aValue;

	while( sourceMapIter.line() ) {
		mapMSBIter.line();
		mapLSBIter.line();

		while( sourceMapIter.pixel() ) {
			mapMSBIter.pixel();
			mapLSBIter.pixel();

			uValue = sourceMapIter.r();
			mapMSBIter.r() = (uint8_t)(uValue >> 8);
			mapLSBIter.r() = (uint8_t)(uValue - mapMSBIter.r() * 256);
			vValue = sourceMapIter.g();
			mapMSBIter.g() = (uint8_t)(vValue >> 8);
			mapLSBIter.g() = (uint8_t)(vValue - mapMSBIter.g() * 256);
			if( sourceMap.hasAlpha() ) {
				aValue = sourceMapIter.a();
				mapMSBIter.a() = (uint8_t)(aValue >> 8);
				mapLSBIter.a() = (uint8_t)(aValue - mapMSBIter.a() * 256);
			}
		}
	}
	*msbTexture = gl::Texture( mapMSB );
	*lsbTexture = gl::Texture( mapLSB );
}

void uvPlayerApp::loadOverlayFile( const fs::path &overlayPath )
{
	try {
		mOverlayTexture = gl::Texture( loadImage( overlayPath ) );
	}
	catch( ... ) {
		console() << "Unable to load overlay file." << endl;
		
		mOverlayTexture.reset();
	};
}
void uvPlayerApp::infoTexture( const string &title )
{
	// create a texture for showing some info about the movie
	TextLayout infoText;
	infoText.clear( ColorA( 0.2f, 0.2f, 0.2f, 0.5f ) );
	infoText.setColor( Color::white() );
	infoText.addCenteredLine( title );
	if( mMovie ) {
		infoText.addLine( toString( mMovie.getWidth() ) + " x " + toString( mMovie.getHeight() ) + " pixels" );
		if( mMovie.getNumFrames() > 0 ) {
			infoText.addLine( toString( mMovie.getDuration() ) + " seconds" );
			infoText.addLine( toString( mMovie.getNumFrames() ) + " frames" );
			infoText.addLine( toString( mMovie.getFramerate() ) + " fps" );
		} else {
			infoText.addLine( "still image" );
		}
	}

	infoText.addCenteredLine( "Keys" );
	infoText.addLine( "space: play/pause" );
	infoText.addLine( "backspace: rewind" );
	infoText.addLine( "o: load movie" );
	infoText.addLine( "m: load uvmap" );
	infoText.addLine( "l: load overlay" );
	infoText.addLine( "e: export processed movie" );
	infoText.addLine( "f: toggle fullscreen" );
	infoText.addLine( "i: toggle info" );
	infoText.addLine( "p: toggle graycode patterns" );

	infoText.setBorder( 4, 2 );
	mInfoTexture = gl::Texture( infoText.render( true ) );
}

Channel uvPlayerApp::graycodes( int aBits ) 
{
	Channel graycodes( (int32_t)pow( (double)2, aBits ), (int32_t)aBits + 1 ); 
	Channel::Iter graycodesIter( graycodes.getIter() );

	uint8_t value;
	int chunkWidth;
	int count;

	while ( graycodesIter.line() ) {
		if ( graycodesIter.y() < aBits ) {
			value = 0;
			
			chunkWidth = (int)pow( (double)2, aBits - graycodesIter.y() );
			count = chunkWidth / 2;
			
			while ( graycodesIter.pixel() ) {
				graycodesIter.v() = value;

				count++;
				if ( count >= chunkWidth ) {
					value = (value == 0) ? -1 : 0;
					count = 0;
				}
			}
		} else {
			while( graycodesIter.pixel() ) {
				graycodesIter.v() = -1 - graycodesIter.v(0, -( aBits - 4 ) );
			}
		}
	}
	return graycodes;
}

CINDER_APP_BASIC( uvPlayerApp, RendererGl(RendererGl::AA_NONE) );