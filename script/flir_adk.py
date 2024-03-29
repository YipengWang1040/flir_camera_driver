import cv2
import argparse
from cv_bridge import CvBridge
from sensor_msgs.msg import Image, TimeReference
import rospy
import time

DEBUG = False
SKIP_FRAME = 5

RED =    '\x1b[1;31m'
YELLOW = '\x1b[1;33m'
GREEN =  '\x1b[1;32m'
WHITE =  '\x1b[0m' 


def init_adk(port):
    return cv2.VideoCapture(port, cv2.CAP_V4L)
        
def finalize_adk(cap):
    cap.release()

def wait_for_image(cap):
    if(cap.grab()):
        return cap.get(cv2.CAP_PROP_POS_MSEC)
    else:
        return 0

def retrive_image(cap):
    success, image = cap.retrieve()
    if(success):
        return image[:,:,0] # since the 3 channels will be exactly same, only use the first one
    else:
        if(DEBUG):
            print(YELLOW+'Unable to retrieve image!'+WHITE)
        return None


def main():
    parser = argparse.ArgumentParser(
                prog='flir_adk_driver',
                description='Read and publish frames from flir adk thermal camera')
    parser.add_argument('port',type=int, default=0)

    args = parser.parse_args()
    
    print(args.port)
    cap = init_adk(args.port)
    if(not cap.isOpened()):
        print(RED+'Unable to open camera!'+WHITE)
        return -1
    
    print(GREEN+'Camera initialized!'+WHITE)

    rospy.init_node('flir_adk_driver')
    pub_image = rospy.Publisher('/flir/adk/image_thermal', Image, queue_size=10)
    pub_time = rospy.Publisher('/flir/adk/time_reference', TimeReference, queue_size=10)
    bridge = CvBridge()
    
    # drop initial frames and calibrate timestamp difference
    prev_time_stamp_ms = 0

    samples = 100
    delta = 0
    for i in range(samples):
        prev_time_stamp_ms = wait_for_image(cap)
        delta += time.time()*1000 - prev_time_stamp_ms
        retrive_image(cap)
    delta /= samples

    skip = 0
    print("Deviec FPS: {}".format(60))
    print("Skipping {} frames, actual FPS: {}".format(SKIP_FRAME, 60/(1+SKIP_FRAME)))
    print(GREEN+'Start publishing......'+WHITE)
    delta = -1
    while(not rospy.is_shutdown()):
        time_stamp_ms = wait_for_image(cap)
        time_stamp_system = rospy.Time.now()

        if(DEBUG):
            print(YELLOW + "delta_t: {}".format(time_stamp_ms-prev_time_stamp_ms)+WHITE)
        prev_time_stamp_ms = time_stamp_ms
        image = retrive_image(cap)
        if(image is None):
            if(DEBUG):
                print(YELLOW + "Skipped empty image at {}".format(time_stamp_ms)+WHITE)
            continue
        
        if(skip==SKIP_FRAME):
            image_msg = bridge.cv2_to_imgmsg(image, encoding='mono8')
            image_msg.header.stamp = time_stamp_system
            image_msg.header.frame_id = "flir/adk/image_thermal"
            pub_image.publish(image_msg)

            time_reference = TimeReference()
            time_reference.header = image_msg.header
            time_reference.time_ref = rospy.Time.from_seconds(time_stamp_ms/1000)
            time_reference.source = "v4l_driver"
            pub_time.publish(time_reference)
            skip=0
        else:
            skip+=1

    finalize_adk(cap)


if __name__ == "__main__":
    main()