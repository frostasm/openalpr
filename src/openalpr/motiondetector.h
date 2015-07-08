
#ifndef OPENALPR_MOTIONDETECTOR_H
#define OPENALPR_MOTIONDETECTOR_H

#include "opencv2/opencv.hpp"
#include "utility.h"

namespace alpr
{
  class MotionDetector
  {
      private:
          cv::Ptr<cv::BackgroundSubtractorMOG2> pMOG2; //MOG2 Background subtractor
          cv::Mat fgMaskMOG2;
          cv::Rect motionRoi;
          bool motionRoiValid;

          int erodeNoiseElementSize;
          bool debugShowMotionImages;

      public:
          MotionDetector(int mogHistory=500, float mogVarThreshold=16, bool mogShadowDetection=false,
                         bool debugShowMotionImages = false);

          virtual ~MotionDetector();

          void ResetMotionDetection(cv::Mat* frame);
          cv::Rect MotionDetect(cv::Mat* frame);

          void setRoi(const cv::Rect &roi);
          cv::Rect roi() const { return motionRoi; }

          void setErodeElementSize(int erodeElementSize) { erodeNoiseElementSize = erodeElementSize;}
          int erodeElementSize() const { return erodeNoiseElementSize; }

  };
}

#endif // OPENALPR_MOTIONDETECTOR_H
