/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (C) 2012 Ken Tossell
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the author nor other contributors may be
*     used to endorse or promote products derived from this software
*     without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

#include "cis_camera/camera_driver.h"

#include <unistd.h>
#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Temperature.h>
#include <std_msgs/Header.h>
#include <image_transport/camera_publisher.h>
#include <dynamic_reconfigure/server.h>
#include <libuvc/libuvc.h>


namespace cis_camera {


CameraDriver::CameraDriver( ros::NodeHandle nh, ros::NodeHandle priv_nh )
    : nh_(nh), priv_nh_(priv_nh),
      state_(kInitial),
      ctx_(NULL), dev_(NULL), devh_(NULL), rgb_frame_(NULL),
      it_(nh_),
      config_server_(mutex_, priv_nh_),
      config_changed_(false),
      cinfo_manager_(nh) {
    
    cam_pub_ = it_.advertiseCamera( "image_raw", 1, false );
}


CameraDriver::~CameraDriver() {
    
    if ( rgb_frame_ )
        uvc_free_frame( rgb_frame_ );
    
    if ( ctx_ )
        uvc_exit( ctx_ );  // Destroys dev_, devh_, etc.
}


bool CameraDriver::Start() {
    
    uvc_error_t err;
    
    err = uvc_init( &ctx_, NULL );
    
    if ( err != UVC_SUCCESS ) {
        ROS_ERROR( "ERROR: uvc_init" );
        return false;
    }
    
    state_ = kStopped;
    
    config_server_.setCallback( boost::bind( &CameraDriver::ReconfigureCallback, this, _1, _2 ) );
    
    return state_ == kRunning;
}


void CameraDriver::Stop() {
    
    boost::recursive_mutex::scoped_lock( mutex_ );
    
    if ( state_ == kRunning )
        CloseCamera();
    
    uvc_exit( ctx_ );
    ctx_ = NULL;
    
    state_ = kInitial;
}


void CameraDriver::ReconfigureCallback( CISCameraConfig &new_config, uint32_t level ) {
    
    boost::recursive_mutex::scoped_lock( mutex_ );
    
    if ( (level & kReconfigureClose) == kReconfigureClose ) {
        if ( state_ == kRunning )
            CloseCamera();
    }
    
    if ( state_ == kStopped ) {
        OpenCamera();
    }
    
    config_ = new_config;
    return;
}


void CameraDriver::ImageCallback( uvc_frame_t *frame ) {
    
    ros::Time timestamp = ros::Time( frame->capture_time.tv_sec, frame->capture_time.tv_usec );
    if ( timestamp == ros::Time(0) ) {
        timestamp = ros::Time::now();
    }
    
    boost::recursive_mutex::scoped_lock(mutex_);
    
    if ( state_ != kRunning || not rgb_frame_ ) {
        return;
    }
    
    int         err;
    int         frame_width;
    int         frame_height;
    double      frame_rate;
    std::string video_mode;
    
    err = priv_nh_.getParam( "width",  frame_width  );
    err = priv_nh_.getParam( "height", frame_height );
    
    sensor_msgs::Image::Ptr image( new sensor_msgs::Image() );
    image->width  = frame_width;
    image->height = frame_height;
    image->step   = image->width * 3;
    image->data.resize( image->step * image->height );
    
    if ( frame->frame_format == UVC_FRAME_FORMAT_GRAY16 ) {
        image->encoding = "16UC1";
        image->step     = image->width*2;
        image->data.resize( image->step * image->height );
        memcpy( &(image->data[0]), frame->data, frame->data_bytes );
        
        // Ad-hoc: Change metric to mm
        uint16_t* data = reinterpret_cast<uint16_t*>(&image->data[0]);
        for ( int i=0; i<image->height*image->width; i++ ) {
            data[i] = (uint16_t)(data[i]*0.406615*4.0);
        }
    }
    else {
        uvc_error_t conv_ret = uvc_any2bgr( frame, rgb_frame_ );
        if ( conv_ret != UVC_SUCCESS ) {
        ROS_ERROR( "Couldn't convert frame to RGB : Error.%d", conv_ret );
        return;
        }
        image->encoding = "bgr8";
        memcpy( &(image->data[0]), rgb_frame_->data, rgb_frame_->data_bytes );
    }
    
    sensor_msgs::CameraInfo::Ptr cinfo( new sensor_msgs::CameraInfo( cinfo_manager_.getCameraInfo() ) );
    
    std::string frame_id;
    err = priv_nh_.getParam( "frame_id",  frame_id );
    
    image->header.frame_id = frame_id;
    image->header.stamp    = timestamp;
    cinfo->header.frame_id = frame_id;
    cinfo->header.stamp    = timestamp;
    
    cam_pub_.publish(image, cinfo);
    TOF_PublishTemperature( frame_id );
    
    if ( config_changed_ ) {
        config_server_.updateConfig( config_ );
        config_changed_ = false;
    }
}

void CameraDriver::ImageCallbackAdapter( uvc_frame_t *frame, void *ptr ) {
    
    CameraDriver *driver = static_cast<CameraDriver*>(ptr);
    
    driver->ImageCallback( frame );
}


enum uvc_frame_format CameraDriver::GetVideoMode( std::string vmode ) {
    
    if( vmode == "uncompressed" ) {
        return UVC_COLOR_FORMAT_UNCOMPRESSED;
    }
    else if ( vmode == "compressed" ) {
        return UVC_COLOR_FORMAT_COMPRESSED;
    }
    else if ( vmode == "yuyv" ) {
        return UVC_COLOR_FORMAT_YUYV;
    } 
    else if ( vmode == "uyvy" ) {
        return UVC_COLOR_FORMAT_UYVY;
    } 
    else if ( vmode == "rgb" ) {
        return UVC_COLOR_FORMAT_RGB;
    } 
    else if ( vmode == "bgr" ) {
        return UVC_COLOR_FORMAT_BGR;
    } 
    else if ( vmode == "mjpeg" ) {
        return UVC_COLOR_FORMAT_MJPEG;
    } 
    else if ( vmode == "gray8" ) {
        return UVC_COLOR_FORMAT_GRAY8;
    } 
    else if ( vmode == "gray16" ) {
        return UVC_COLOR_FORMAT_GRAY16;
    } 
    else {
        ROS_ERROR_STREAM( "Invalid Video Mode: " << vmode );
        ROS_WARN_STREAM( "Continue using video mode: uncompressed" );
        return UVC_COLOR_FORMAT_UNCOMPRESSED;
    }
};


void CameraDriver::OpenCamera() {
    
    int err;
    int vendor_id  = 0;
    int product_id = 0;
    int index_id   = 0;
    
    std::string param_st;
    
    std::string serial_id  = "0";
    
    err = priv_nh_.getParam( "vendor" , param_st );
    vendor_id  = strtol( param_st.c_str(), NULL, 0 );
    
    err = priv_nh_.getParam( "product", param_st );
    product_id = strtol( param_st.c_str(), NULL, 0 );
    
    err = priv_nh_.getParam( "serial" , param_st );
    serial_id  = param_st;
    
    err = priv_nh_.getParam( "index"  , param_st );
    index_id   = strtol( param_st.c_str(), NULL, 0 );
    
    ROS_INFO("Opening camera with vendor=0x%x, product=0x%x, serial=\"%s\", index=%d",
                vendor_id, product_id, serial_id.c_str(), index_id );
    
    uvc_device_t **devs;
    
    uvc_error_t find_err = uvc_find_devices(
        ctx_, &devs,
        vendor_id,
        product_id,
        serial_id.empty() ? NULL : serial_id.c_str() );
    
    if ( find_err != UVC_SUCCESS ) {
        ROS_ERROR( "uvc_find_device" );
        return;
    }
    
    // select device by index
    dev_ = NULL;
    int dev_idx = 0;
    while ( devs[dev_idx] != NULL ) {
        if( dev_idx == index_id ) {
            dev_ = devs[dev_idx];
        }
        else {
            uvc_unref_device( devs[dev_idx] );
        }
        
        dev_idx++;
    }
    
    if ( dev_ == NULL ) {
        ROS_ERROR( "Unable to find device at index %d", index_id );
        return;
    }
    
    uvc_error_t open_err = uvc_open( dev_, &devh_ );
    
    if ( open_err != UVC_SUCCESS ) {
        switch ( open_err ) {
            case UVC_ERROR_ACCESS:
#ifdef __linux__
                ROS_ERROR( "Permission denied opening /dev/bus/usb/%03d/%03d",
                            uvc_get_bus_number( dev_ ), uvc_get_device_address( dev_ ) );
#else
                ROS_ERROR( "Permission denied opening device %d on bus %d",
                            uvc_get_device_address( dev_ ), uvc_get_bus_number( dev_ ) );
#endif
                break;
            default:
#ifdef __linux__
                ROS_ERROR( "Can't open /dev/bus/usb/%03d/%03d: %s (%d)",
                            uvc_get_bus_number( dev_ ), uvc_get_device_address( dev_ ),
                            uvc_strerror( open_err ), open_err );
#else
                ROS_ERROR( "Can't open device %d on bus %d: %s (%d)",
                            uvc_get_device_address( dev_ ), uvc_get_bus_number( dev_ ),
                            uvc_strerror( open_err ), open_err );
#endif
                break;
        }
        
        uvc_unref_device( dev_ );
        return;
    }
    
    int         frame_width  = 640;
    int         frame_height = 480;
    double      frame_rate   = 1000.0;
    std::string video_mode   = "uncompressed";
    
    err = priv_nh_.getParam( "width" , frame_width  );
    err = priv_nh_.getParam( "height" , frame_height );
    err = priv_nh_.getParam( "frame_rate" , frame_rate  );
    err = priv_nh_.getParam( "video_mode" , video_mode  );
    
    uvc_stream_ctrl_t ctrl;
    
    uvc_error_t mode_err = uvc_get_stream_ctrl_format_size(
        devh_, &ctrl,
        GetVideoMode( video_mode ),
        frame_width, frame_height,
        frame_rate );
    
    if (mode_err != UVC_SUCCESS) {
        ROS_ERROR( "uvc_get_stream_ctrl_format_size");
        uvc_close(devh_);
        uvc_unref_device(dev_);
        ROS_ERROR("check video_mode/width/height/frame_rate are available");
        uvc_print_diag(devh_, NULL);
        return;
    }
    
    uvc_error_t stream_err = uvc_start_streaming( devh_, &ctrl,
                                                  &CameraDriver::ImageCallbackAdapter, 
                                                  this, 0 );
    
    if ( stream_err != UVC_SUCCESS ) {
        ROS_ERROR( "uvc_start_streaming");
        uvc_close( devh_ );
        uvc_unref_device( dev_ );
        return;
    }
    
    if ( rgb_frame_ )
        uvc_free_frame( rgb_frame_ );
    
    rgb_frame_ = uvc_allocate_frame( frame_width * frame_height * 3 );
    
    std::string camera_info_url = "";
    err = priv_nh_.getParam( "camera_info_url" , camera_info_url  );
    cinfo_manager_.loadCameraInfo( camera_info_url );
    
    // TOF Camera Settigns
    int tof_err;
    tof_err = TOF_SetEEPROMMode( TOF_EEPROM_FACTORY_DEFAULT );
    tof_err = TOF_ClearError();
    TOF_SetMode_All();
    
    // Get TOF Camera Informations
    TOF_GetInfo_All();
    
    // Set Publishers for TOF Camera Temperature
    std::string node_name = ros::this_node::getName();
    tof_t1_pub_ = nh_.advertise<sensor_msgs::Temperature>( node_name + "/t1", 1000 );
    tof_t2_pub_ = nh_.advertise<sensor_msgs::Temperature>( node_name + "/t2", 1000 );
    
    state_ = kRunning;
}


int CameraDriver::TOF_SetCtrl( uint16_t *data ,int size ) {
    
    int err;
        
    err = uvc_set_ctrl( devh_, 3, 0x03, data, size );
    if ( err != size ) {
        ROS_ERROR( "Set Ctrl failed. Error: %d", err );
    }
    return err;
}


int CameraDriver::TOF_GetCtrl( uint16_t *data, int size ) {
    
    int err;
    
    err = TOF_SetCtrl( data, size );
    if ( err != size ) {
        ROS_ERROR( "Set Ctrl to Get failed : Error: %d", err );
        return err;
    }
    else {
        err = uvc_get_ctrl( devh_, 3, 0x03, data, size, UVC_GET_CUR );
        if ( err != size ) {
            ROS_ERROR( "Get Ctrl failed. Error: %d", err );
        }
    }
    return err;
}


void CameraDriver::TOF_SetMode_All() {
    
    int err;
    std::string rp_name;
    std::string rosparam_names[8] =
    {
        "depth_ir",
        "depth_range",
        "threshold",
        "nr_filter",
        "pulse_count",
        "ld_enable",
        "ir_gain",
        "error_stop"
    };
    
    int name_num = sizeof( rosparam_names ) / sizeof( rosparam_names[0] );
    for ( int i = 0; i < name_num ; i++ ) {
        rp_name = rosparam_names[i];
        ROS_INFO( "%d. ROS Param : %s", i, rp_name.c_str() );
        err = TOF_SetMode_ROSParameter( rosparam_names[i] );
    }
    
    return;
}


int CameraDriver::TOF_SetMode_ROSParameter( std::string param_name ) {
    
    uint16_t send[5] = { TOF_SET_DEPTH_IR, 0, 0, 0, 0 };
    uint16_t recv[5] = { TOF_GET_DEPTH_IR, 0, 0, 0, 0 };
    
    int param_set[5] = { TOF_SET_DEPTH_IR, 0, 0, 0, 0 };
    int param_min[5] = { 0x0000, 0, 0, 0, 0 };
    int param_max[5] = { 0xFFFF, 1, 1, 1, 1 };
    
    int param = 0;
    int err   = 0;
    
    // Get ROS Parameter and Set Data
    err   = priv_nh_.getParam( param_name, param );
    if ( err ) {
        
        if ( param_name == "depth_ir" ) {
            
            param_set[0] = TOF_SET_DEPTH_IR;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 1;
            
            recv[0] = TOF_GET_DEPTH_IR;
        }
        else if ( param_name == "depth_range" ) {
            
            param_set[0] = TOF_SET_DEPTH_RANGE;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 1;
            
            recv[0] = TOF_GET_DEPTH_RANGE;
        }
        else if ( param_name == "threshold" ) {
            
            param_set[0] = TOF_SET_THRESHOLD;
            param_set[1] = param;
            
            param_min[1] = 0x0000;
            param_max[1] = 0x3FFF;
            
            recv[0] = TOF_GET_THRESHOLD;
        }
        else if ( param_name == "nr_filter" ) {
            
            param_set[0] = TOF_SET_NR_FILTER;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 1;
            
            recv[0] = TOF_GET_NR_FILTER;
        }
        else if ( param_name == "pulse_count" ) {
            
            param_set[0] = TOF_SET_PULSE_COUNT;
            param_set[1] = param;
            
            param_min[1] = 1;
            param_max[1] = 2000;
            
            recv[0] = TOF_GET_PULSE_COUNT;
        }
        else if ( param_name == "ld_enable" ) {
            
            param_set[0] = TOF_SET_LD_ENABLE;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 15;
            
            recv[0] = TOF_GET_LD_ENABLE;
        }
        else if ( param_name == "ir_gain" ) {
            
            param_set[0] = TOF_SET_IR_GAIN;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 0x07FF;
            
            recv[0] = TOF_GET_IR_GAIN;
        }
        else if ( param_name == "error_stop" ) {
            
            param_set[0] = TOF_SET_ERROR_STOP;
            param_set[1] = param;
            
            param_min[1] = 0;
            param_max[1] = 1;
            
            recv[0] = TOF_GET_ERROR_STOP;
        }
        else {
            ROS_WARN( "Unmatch Parameter Name : %s", param_name.c_str() );
            err = 0;
            return err;
        }
        
        // Value Check
        for ( int i = 0; i < 5; i++ ) {
            if ( param_set[i] < param_min[i] )      send[i] = param_min[i];
            else if ( param_max[i] < param_set[i] ) send[i] = param_max[i];
            else                                    send[i] = param_set[i];
        }
    }
    else {
        ROS_ERROR( "Parameter Acquisition Error : %s", param_name.c_str() );
        return err;
    }
    
    // Set Parameter on TOF Camera
    err = TOF_SetCtrl( send, sizeof(send) );
    if ( err == sizeof(send) ) {
        ROS_INFO( "Set Parameter %s as { %d, %d, %d, %d } on TOF Camera", 
                                param_name.c_str(), send[1], send[2], send[3], send[4] );
    }
    else {
        ROS_ERROR( "Set Parameter %s failed. Error: %d", param_name.c_str(), err );
        return err;
    }
    
    // Get Parameter on TOF Camera for Check
    err = TOF_GetCtrl( recv, sizeof(recv) );
    if ( err == sizeof(recv) ) {
        ROS_INFO( "Get Parameter %s as { %d, %d, %d, %d } on TOF Camera", 
                                param_name.c_str(), recv[1], recv[2], recv[3], recv[4] );
    }
    else {
        ROS_ERROR( "Get Parameter of %s for Check Failed. Error : %d", param_name.c_str(), err );
        return err;
    }
    
    return err;
}


int CameraDriver::TOF_SetEEPROMMode( uint16_t mode = TOF_EEPROM_FACTORY_DEFAULT ) {
    
    uint16_t send[5] = { TOF_SET_EEPROM, 0, 0, 0, 0 };
    int err;
    
    uint16_t val_min = 0x0000;
    uint16_t val_max = 0x0001;
    
    // Value Check
    if ( mode < val_min )      send[1] = val_min;
    else if ( val_max < mode ) send[1] = val_max;
    else                       send[1] = mode;
    
    err = TOF_SetCtrl( send, sizeof(send) );
    if ( err == sizeof(send) ) {
        ROS_INFO( "Set EEPROM Mode : %d", send[1] );
    }
    else {
        ROS_ERROR( "Set EEPROM Mode failed. Error: %d", err );
        return err;
    }
    
    return err;
}


int CameraDriver::TOF_ClearError() {
    
    uint16_t send[5] = { TOF_SET_ERROR_CLEAR, 0, 0, 0, 0 };
    int err;
    
    err = TOF_SetCtrl( send, sizeof(send) );
    if ( err == sizeof(send) ) {
        ROS_INFO( "Clear TOF Camera Errors" );
    }
    else {
        ROS_ERROR( "Clear TOF Camera Errors failed. Error: %d", err );
        return err;
    }
    
    return err;
}


void CameraDriver::TOF_GetInfo_All() {
    
    int tof_err;
    
    // Get TOF Camera Informations
    
    uint16_t version_n;
    uint16_t build_n;
    uint16_t build_y;
    uint16_t build_d;
    tof_err = TOF_GetVersion( version_n, build_n, build_y, build_d );
    
    uint16_t depth_ir;
    tof_err = TOF_GetDepthIR( depth_ir );
    
    uint16_t depth_range;
    uint16_t dr_index;
    tof_err = TOF_GetDepthRange( depth_range, dr_index );
    
    uint16_t threshold;
    tof_err = TOF_GetThreshold( threshold );
    
    uint16_t nr_filter;
    tof_err = TOF_GetNRFilter( nr_filter );
    
    uint16_t pulse_count;
    tof_err = TOF_GetPulseCount( pulse_count );
    
    uint16_t ld_enable;
    tof_err = TOF_GetLDEnable( ld_enable );
    
    double depth_cnv_gain;
    tof_err = TOF_GetDepthCnvGain( depth_cnv_gain );
    
    short          offset_val;
    unsigned short max_data;
    unsigned short min_dist;
    unsigned short max_dist;
    tof_err = TOF_GetDepthInfo( offset_val, max_data, min_dist, max_dist );
    
    uint16_t ir_gain;
    tof_err = TOF_GetIRGain( ir_gain );
    
    double t1;
    double t2;
    tof_err = TOF_GetTemperature( t1, t2 );
    ROS_INFO( "Get Temperature T1 : %.1f / T2 : %.1f [deg C]", t1, t2 );
    
    uint16_t error_stop;
    tof_err = TOF_GetErrorStop( error_stop );
    
    uint16_t common_err;
    uint16_t eeprom_err_factory; 
    uint16_t eeprom_err;
    uint16_t mipi_temp_err;
    tof_err = TOF_GetErrorInfo( common_err, eeprom_err_factory, eeprom_err, mipi_temp_err );
    
    return;
}


int CameraDriver::TOF_GetDepthIR( uint16_t& depth_ir ) {
    
    uint16_t data[5] = { TOF_GET_DEPTH_IR, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        depth_ir = data[1];
        ROS_INFO( "Get Depth/IR Mode : %d", depth_ir );
    }
    else {
        ROS_ERROR( "Get Depth IR Mode failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetDepthRange( uint16_t& depth_range, uint16_t& dr_index ) {
    
    uint16_t data[5] = { TOF_GET_DEPTH_RANGE, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        depth_range = data[1];
        dr_index    = data[2];
        ROS_INFO( "Get Depth Range Mode : %d / Index : %d", depth_range, dr_index );
    }
    else {
        ROS_ERROR( "Get Depth  Range Mode failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetThreshold( uint16_t& threshold ) {
    
    uint16_t data[5] = { TOF_GET_THRESHOLD, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        threshold = data[1];
        ROS_INFO( "Get Threshold : %d", threshold );
    }
    else {
        ROS_ERROR( "Get Threshold failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetNRFilter( uint16_t& nr_filter ) {
    
    uint16_t data[5] = { TOF_GET_NR_FILTER, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        nr_filter = data[1];
        ROS_INFO( "Get NR Filter : %d", nr_filter );
    }
    else {
        ROS_ERROR( "Get NR Filter failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetPulseCount( uint16_t& pulse_count ) {
    
    uint16_t data[5] = { TOF_GET_PULSE_COUNT, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        pulse_count = data[1];
        ROS_INFO( "Get Pulse Count : %d", pulse_count );
    }
    else {
        ROS_ERROR( "Get Pulse Count failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetLDEnable( uint16_t& ld_enable ) {
    
    uint16_t data[5] = { TOF_GET_LD_ENABLE, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        ld_enable = data[1];
        ROS_INFO( "Get LD Enable : %d", ld_enable );
    }
    else {
        ROS_ERROR( "Get LD Enable failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetDepthCnvGain( double& depth_cnv_gain ) {
    
    uint16_t data[5] = { TOF_GET_DEPTH_CNV_GAIN, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        depth_cnv_gain = *(double*)(&data[1]);
        ROS_INFO( "Get Depth Cnv Gain : %f", depth_cnv_gain );
    }
    else {
        ROS_ERROR( "Get Depth Cnv Gain failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetDepthInfo( short&          offset_val, 
                                    unsigned short& max_data, 
                                    unsigned short& min_dist, 
                                    unsigned short& max_dist  ) {
    
    uint16_t data[5] = { TOF_GET_DEPTH_INFO, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        offset_val = *(short*)(&data[1]);
        max_data   = *(unsigned short*)(&data[2]);
        min_dist   = *(unsigned short*)(&data[3]);
        max_dist   = *(unsigned short*)(&data[4]);
        ROS_INFO( "Get Depth Info - Offset: %d / Max Data : %d / min Distance : %d [mm] MAX Distance :%d [mm]", 
                    offset_val, max_data, min_dist, max_dist );
    }
    else {
        ROS_ERROR( "Get Depth Info failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetIRGain( uint16_t& ir_gain ) {
    
    uint16_t data[5] = { TOF_GET_IR_GAIN, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        ir_gain = data[1];
        ROS_INFO( "Get IR Gain : %d", ir_gain );
    }
    else {
        ROS_ERROR( "Get IR Gain failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetTemperature( double& t1, double& t2 ) {
    
    uint16_t data[5] = { TOF_GET_TEMPERATURE, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        t1 = data[1] / 256.0;
        t2 = data[2] / 256.0;
        // ROS_INFO( "Get Temperature T1 : %.1f / T2 : %.1f [deg C]", t1, t2 );
    }
    else {
        ROS_ERROR( "Get Temperature failed. Error: %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetErrorStop( uint16_t& error_stop ) {
    
    uint16_t data[5] = { TOF_GET_ERROR_STOP, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        error_stop = data[1];
        ROS_INFO( "Get Error Stop : %d", error_stop );
    }
    else {
        ROS_ERROR( "Get Error Stop failed. Error : %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetVersion( uint16_t& version_n, 
                                  uint16_t& build_n, 
                                  uint16_t& build_y, 
                                  uint16_t& build_d   ) {
    
    uint16_t data[5] = { TOF_GET_VERSION, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        version_n = data[1];
        build_n   = data[2];
        build_y   = data[3];
        build_d   = data[4];
        ROS_INFO( "Get Version : %x / Build : %x / Build Date : %x%x (YYYYMMDD)", 
                    version_n, build_n, build_y, build_d );
    }
    else {
        ROS_ERROR( "Get Version failed. Error: %d", err );
    }
    
    return err;
}


int CameraDriver::TOF_GetErrorInfo( uint16_t& common_err, 
                                    uint16_t& eeprom_err_factory, 
                                    uint16_t& eeprom_err, 
                                    uint16_t& mipi_temp_err     ) {
    
    uint16_t data[5] = { TOF_GET_ERROR_INFO, 0, 0, 0, 0 };
    int err;
    
    err = TOF_GetCtrl( data, sizeof(data) );
    if ( err == sizeof(data) ) {
        common_err          = data[1];
        eeprom_err_factory  = data[2];
        eeprom_err          = data[3];
        mipi_temp_err       = data[4];
        ROS_INFO( "Get Error Info - Common : 0x%02x / EEPROM Factory : 0x%02x / EEPROM : 0x%02x / MIPI/Temperature : 0x%02x", 
                    common_err, eeprom_err_factory, eeprom_err, mipi_temp_err );
    }
    else {
        ROS_ERROR( "Get Error Info failed. Error: %d", err );
    }
    
    return err;
}


void CameraDriver::TOF_PublishTemperature( std::string frame_id ) {
    
    sensor_msgs::Temperature t_msg;
    
    double t1;
    double t2;
    
    TOF_GetTemperature( t1, t2 );
    
    t_msg.header.frame_id = frame_id;
    t_msg.header.stamp    = ros::Time::now();
    
    t_msg.temperature = t1;
    tof_t1_pub_.publish( t_msg );
    
    t_msg.temperature = t2;
    tof_t2_pub_.publish( t_msg );
    
    return;
}


void CameraDriver::CloseCamera() {
    
    uvc_close( devh_ );
    devh_ = NULL;
    
    uvc_unref_device( dev_ );
    dev_ = NULL;
    
    state_ = kStopped;
}

};