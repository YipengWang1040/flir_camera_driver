#include <iostream>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <thread>

#include <ros/ros.h>
#include <std_msgs/Header.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/TimeReference.h>
#include <cv_bridge/cv_bridge.h>

#include "device.h"
#include "config.h"
#include "utils.hpp"

//#include "../../devel/include/flir_camera_driver/ImageAddition.h"   // incase where IDE cannot correctly find the generated message
#include "flir_camera_driver/ImageAddition.h"

using namespace std;

static int skip=0;
static string topic_rgb="/blackfly/cam%/image_color";
static string topic_raw="/blackfly/cam%/image_raw";
static string topic_stamp="/blackfly/cam%/time_reference";
static string topic_addition="/blackfly/additional";

static bool run=true;

void sigint_handler(int){
    if(run){
        ROS_INFO("SIGINT recieved. Exiting....");
        run=false;
    }
}

void apply_configs(std::list<Device>& cameras, configure::config_reader& reader){
    for(auto&& camera:cameras){
        for (auto&& config : reader.get_camera_configs()) {
            if (!camera.configure(config)) {
                ROS_FATAL("Failed to config camera at %s", config.field.c_str());
            }
        }
    }
    skip=reader.get("skip",0);
    topic_raw=reader.get("ros_topic_raw",string("/blackfly/cam%/image_raw"));
    topic_rgb=reader.get("ros_topic_color",string("/blackfly/cam%/image_color"));
    topic_stamp=reader.get("ros_topic_time",string("/blackfly/cam%/time_reference"));
    topic_addition=reader.get("additional_topic",string("/blackfly/additional"));
}

void run_cam(Device& camera, size_t idx, ros::Publisher& pub_raw, ros::Publisher& pub_rgb, ros::Publisher& pub_time, ros::Publisher& pub_addition){
    int counter=skip;
    while(run){
        cv::Mat image_rgb;
        cv::Mat image_raw;
        size_t time_stamp_device;
        size_t time_stamp_system;
        double exposure_time, gain;
        if(camera.grab(image_raw,image_rgb,time_stamp_device, time_stamp_system, exposure_time,gain)){

            // skip publishing if 'skip' was set
            // this could be used in calibration where lower framerate is sufficient
            counter+=1;
            if(counter<=skip)
                continue;
            counter=0;

            std::string frame_id = strfmt("flir_blackfly_cam%", idx);

            if(!image_rgb.empty()){
                cv_bridge::CvImage cv_image;
                cv_image.header.stamp.fromNSec(time_stamp_system);
                cv_image.header.frame_id=frame_id;
                cv_image.encoding="bgr8";
                cv_image.image=image_rgb;
                pub_rgb.publish(cv_image.toImageMsg());
            }

            if(!image_raw.empty()){
                cv_bridge::CvImage cv_image;
                cv_image.header.stamp.fromNSec(time_stamp_system);
                cv_image.header.frame_id=frame_id;
                if(image_raw.type()==CV_8UC3){
                    cv_image.encoding="bgr8";
                }
                else{
                    cv_image.encoding="mono8";
                }
                cv_image.image=image_raw;
                pub_raw.publish(cv_image.toImageMsg());
            }

            if(!image_raw.empty() || !image_rgb.empty()){
                flir_camera_driver::ImageAddition addition;
                addition.header.stamp.fromNSec(time_stamp_system);
                addition.header.frame_id=frame_id;
                addition.exposure=exposure_time;
                addition.gain=gain;
                pub_addition.publish(addition);

                sensor_msgs::TimeReference reference;
                reference.header = addition.header;
                reference.time_ref.fromNSec(time_stamp_device);
                reference.source = "hardware";
                pub_time.publish(reference);
            }

            usleep(10000);
        }
        else{
            usleep(100000);
        }
    }
}

int main(int argc, char* argv[]){
    ros::init(argc, argv, "flir_driver");
    ros::NodeHandle nh("~");

    signal(SIGINT,sigint_handler);

    Spinnaker::SystemPtr system=Spinnaker::System::GetInstance();
    auto camera_list=Device::enumerate();

    for (auto&& camera : camera_list) {
        if (!camera.default_initialization()) {
            ROS_FATAL("Failed to initialze the camera!");
            return -1;
        }
    }

    string config_file=nh.param("config_file",string("src/myflir/config/default.yaml"));
    configure::config_reader reader(config_file);
    apply_configs(camera_list,reader);

    ROS_INFO("starting");

    // initialize publishers and threads handles
    std::vector<ros::Publisher> pubs_image_raw(camera_list.size());
    std::vector<ros::Publisher> pubs_image_rgb(camera_list.size());
    std::vector<ros::Publisher> pubs_image_time(camera_list.size());
    std::vector<ros::Publisher> pub_addition(camera_list.size());
    std::vector<std::thread> working_threads(camera_list.size());
    size_t idx=0;
    for(auto&& camera:camera_list){
        pubs_image_raw[idx]=(nh.advertise<sensor_msgs::Image>(strfmt(topic_raw.c_str(),idx),10));
        pubs_image_rgb[idx]=(nh.advertise<sensor_msgs::Image>(strfmt(topic_rgb.c_str(),idx),10));
        pubs_image_time[idx]=(nh.advertise<sensor_msgs::TimeReference>(strfmt(topic_stamp.c_str(),idx),10));
        pub_addition[idx]=(nh.advertise<flir_camera_driver::ImageAddition>(strfmt(topic_addition.c_str(),idx),10));
        working_threads[idx]=(thread{&run_cam,std::ref(camera),idx,std::ref(pubs_image_raw[idx]),std::ref(pubs_image_rgb[idx]), std::ref(pubs_image_time[idx]),std::ref(pub_addition[idx])});
        idx+=1;
    }

    // wait for threads to complete
    for(auto&& thread:working_threads){
        thread.join();
    }

    for(auto&& camera:camera_list){
        camera.clear();
    }
    system->ReleaseInstance();
    return 0;
}
