/*
 * Copyright (c) 2015 OpenALPR Technology, Inc.
 * Open source Automated License Plate Recognition [http://www.openalpr.com]
 * 
 * This file is part of OpenALPR.
 * 
 * OpenALPR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License 
 * version 3 as published by the Free Software Foundation 
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef OPENALPR_CONFIG_H
#define OPENALPR_CONFIG_H


#include "simpleini/simpleini.h"
#include "support/filesystem.h"
#include "support/platform.h"

#include "constants.h"

#include <stdio.h>
#include <iostream>
#include <stdlib.h>     /* getenv */
#include <math.h>

namespace alpr
{

  class Config
  {

    public:
      Config(const std::string country, const std::string config_file = "", const std::string runtime_dir = "");
      virtual ~Config();

      bool loaded;

      std::string country;

      int detector;

      float detection_iteration_increase;
      int detectionStrictness;
      float maxPlateWidthPercent;
      float maxPlateHeightPercent;
      int maxDetectionInputWidth;
      int maxDetectionInputHeight;
      
      int platesRoiX;
      int platesRoiY;
      int platesRoiWidth;
      int platesRoiHeight;

      bool skipDetection;

      std::string prewarp;
      
      int maxPlateAngleDegrees;

      float minPlateSizeWidthPx;
      float minPlateSizeHeightPx;

      bool multiline;

      float plateWidthMM;
      float plateHeightMM;

      float charHeightMM;
      float charWidthMM;
      float charWhitespaceTopMM;
      float charWhitespaceBotMM;

      int templateWidthPx;
      int templateHeightPx;

      int ocrImageWidthPx;
      int ocrImageHeightPx;

      int stateIdImageWidthPx;
      int stateIdimageHeightPx;

      float charAnalysisMinPercent;
      float charAnalysisHeightRange;
      float charAnalysisHeightStepSize;
      int charAnalysisNumSteps;

      float plateLinesSensitivityVertical;
      float plateLinesSensitivityHorizontal;

      int segmentationMinBoxWidthPx;
      float segmentationMinCharHeightPercent;
      float segmentationMaxCharWidthvsAverage;

      std::string ocrLanguage;
      int ocrMinFontSize;

      float postProcessMinConfidence;
      float postProcessConfidenceSkipLevel;
      unsigned int postProcessMinCharacters;
      unsigned int postProcessMaxCharacters;

      bool debugGeneral;
      bool debugTiming;
      bool debugPrewarp;
      bool debugDetector;
      bool debugStateId;
      bool debugPlateLines;
      bool debugPlateCorners;
      bool debugCharSegmenter;
      bool debugCharAnalysis;
      bool debugColorFiler;
      bool debugOcr;
      bool debugPostProcess;
      bool debugShowImages;
      bool debugPauseOnFrame;

      void debugOff();

      std::string getKeypointsRuntimeDir();
      std::string getCascadeRuntimeDir();
      std::string getPostProcessRuntimeDir();
      std::string getTessdataPrefix();

    private:
    
      float ocrImagePercent;
      float stateIdImagePercent;
    
      std::string runtimeBaseDir;

      void loadCommonValues(std::string configFile);
      void loadCountryValues(std::string configFile, std::string country);

      int getInt(CSimpleIniA* ini, std::string section, std::string key, int defaultValue);
      float getFloat(CSimpleIniA* ini, std::string section, std::string key, float defaultValue);
      std::string getString(CSimpleIniA* ini, std::string section, std::string key, std::string defaultValue);
      bool getBoolean(CSimpleIniA* ini, std::string section, std::string key, bool defaultValue);
  };


  enum DETECTOR_TYPE
  {
    DETECTOR_LBP_CPU=0,
    DETECTOR_LBP_GPU=1,
    DETECTOR_MORPH_CPU=2
  };

}
#endif // OPENALPR_CONFIG_H
