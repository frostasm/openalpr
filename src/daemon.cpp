

#include <unistd.h>
#include <sstream>
#include <execinfo.h>

#include "daemon/beanstalk.hpp"
#include "video/logging_videobuffer.h"
#include "motiondetector.h"

#include "tclap/CmdLine.h"
#include "alpr.h"
#include "openalpr/simpleini/simpleini.h"
#include "openalpr/cjson.h"
#include "support/tinythread.h"
#include <curl/curl.h>
#include "support/timing.h"

#include <log4cplus/logger.h>
#include <log4cplus/loggingmacros.h>
#include <log4cplus/configurator.h>
#include <log4cplus/consoleappender.h>
#include <log4cplus/fileappender.h>

#include <string>
#include <atomic>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

template <typename T>
class Queue
{
 public:

  bool pop(T& item)
  {
    std::unique_lock<std::mutex> mlock(mutex_);
    if(queue.empty()) {
        return false;
    }
    else
    {
        item = queue.front();
        queue.pop();
        return true;
    }
  }

  void push(const T& item)
  {
    std::unique_lock<std::mutex> mlock(mutex_);

    if(queue.size() > maximumSize)
        removeOddItems();

    queue.push(item);

    mlock.unlock();
    cond_.notify_one();
  }

  Queue()=default;
  Queue(const Queue&) = delete;            // disable copying
  Queue& operator=(const Queue&) = delete; // disable assignment

 private:

  static const int maximumSize = 101; // ! Must be odd
  void removeOddItems() {
      static_assert(maximumSize % 2 == 1, "Mmaximum queue size must be odd");
      static_assert(maximumSize > 50, "Mmaximum queue size to small. must be > 50");
      std::cout << "Queue reached its maximum size: " << maximumSize << std::endl;
      std::queue<T> qOdd;
      while(!queue.empty()) {
          qOdd.push(queue.front());
          queue.pop();
          queue.pop();
      }

      queue.swap(qOdd);
  }

  std::queue<T> queue;
  std::mutex mutex_;
  std::condition_variable cond_;
};

typedef std::pair<cv::Mat, cv::Rect> RecognitionData;
typedef Queue<RecognitionData> RecognitionQueue;

using namespace alpr;

// prototypes
void streamCaptureThread(void* arg);
void platesRecognitionThread(void* arg);
bool writeToQueue(std::string jsonResult);
bool uploadPost(CURL* curl, std::string url, std::string data);
void dataUploadThread(void* arg);

// Constants
const std::string ALPRD_CONFIG_FILE_NAME="alprd.conf";
const std::string OPENALPR_CONFIG_FILE_NAME="openalpr.conf";
const std::string DEFAULT_LOG_FILE_PATH="/var/log/alprd.log";

const std::string BEANSTALK_QUEUE_HOST="127.0.0.1";
const int BEANSTALK_PORT=11300;
const std::string BEANSTALK_TUBE_NAME="alprd";


struct CaptureThreadData
{
  RecognitionQueue recognitionQueues[2];
  int recognitionThreadsCount;

  std::string company_id;
  std::string stream_url;
  std::string site_id;
  int camera_id;
  
  bool clock_on;
  
  std::string config_file;
  std::string country_code;
  bool output_images;
  std::string output_image_folder;
  int top_n;

  bool detect_motion;

  int motion_mog_history_size;
  double motion_mog_var_threshold;
  bool motion_mog_detect_shadows;
  int motion_noise_erase_element_size;
  bool motion_debug_show_images;

  int motion_roi_x;
  int motion_roi_y;
  int motion_roi_width;
  int motion_roi_height;


};

CaptureThreadData* makeCaptureThreadDataFromIni(const CSimpleIniA &ini) {
    CaptureThreadData* tdata = new CaptureThreadData();

    tdata->country_code = ini.GetValue("daemon", "country", "us");;
    tdata->top_n = ini.GetLongValue("daemon", "topn", 20);

    tdata->output_images = ini.GetBoolValue("daemon", "store_plates", false);
    tdata->output_image_folder = ini.GetValue("daemon", "store_plates_location", "/tmp/");
    tdata->company_id = ini.GetValue("daemon", "company_id", "");
    tdata->site_id = ini.GetValue("daemon", "site_id", "");

    tdata->detect_motion = ini.GetBoolValue("daemon", "detect_motion", false);

    tdata->motion_roi_x = ini.GetLongValue("daemon", "motion_roi_x", 0);
    tdata->motion_roi_y = ini.GetLongValue("daemon", "motion_roi_y", 0);
    tdata->motion_roi_width = ini.GetLongValue("daemon", "motion_roi_width", 0);
    tdata->motion_roi_height = ini.GetLongValue("daemon", "motion_roi_height", 0);

    tdata->motion_mog_history_size = ini.GetLongValue("daemon", "motion_mog_history_size", 500);
    tdata->motion_mog_var_threshold = ini.GetDoubleValue("daemon", "motion_mog_var_threshold", 16.0);
    tdata->motion_mog_detect_shadows = ini.GetBoolValue("daemon", "motion_mog_detect_shadows", false);
    tdata->motion_noise_erase_element_size = ini.GetLongValue("daemon", "motion_noise_erase_element_size", 200);

    tdata->motion_debug_show_images = ini.GetBoolValue("daemon", "motion_debug_show_images", false);

    return tdata;
}

struct UploadThreadData
{
  std::string upload_url;
};

void segfault_handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}

bool daemon_active;

static log4cplus::Logger logger;

int main( int argc, const char** argv )
{
  signal(SIGSEGV, segfault_handler);   // install our segfault handler
  daemon_active = true;

  bool noDaemon = false;
  bool clockOn = false;
  std::string logFile;
  
  std::string configDir;

  TCLAP::CmdLine cmd("OpenAlpr Daemon", ' ', Alpr::getVersion());

  TCLAP::ValueArg<std::string> configDirArg("","config","Path to the openalpr config directory that contains alprd.conf and openalpr.conf. (Default: /etc/openalpr/)",false, "/etc/openalpr/" ,"config_file");
  TCLAP::ValueArg<std::string> logFileArg("l","log","Log file to write to.  Default=" + DEFAULT_LOG_FILE_PATH,false, DEFAULT_LOG_FILE_PATH ,"topN");

  TCLAP::SwitchArg daemonOffSwitch("f","foreground","Set this flag for debugging.  Disables forking the process as a daemon and runs in the foreground.  Default=off", cmd, false);
  TCLAP::SwitchArg clockSwitch("","clock","Display timing information to log.  Default=off", cmd, false);

  try
  {
    
    cmd.add( configDirArg );
    cmd.add( logFileArg );

    
    if (cmd.parse( argc, argv ) == false)
    {
      // Error occured while parsing.  Exit now.
      return 1;
    }

    // Make sure configDir ends in a slash
    configDir = configDirArg.getValue();
    if (hasEnding(configDir, "/") == false)
      configDir = configDir + "/";
    
    logFile = logFileArg.getValue();
    noDaemon = daemonOffSwitch.getValue();
    clockOn = clockSwitch.getValue();
  }
  catch (TCLAP::ArgException &e)    // catch any exceptions
  {
    std::cerr << "error: " << e.error() << " for arg " << e.argId() << std::endl;
    return 1;
  }
  
  std::string openAlprConfigFile = configDir + OPENALPR_CONFIG_FILE_NAME;
  std::string openAlprConfigFile1 = configDir + OPENALPR_CONFIG_FILE_NAME+"1";
  std::string openAlprConfigFile2 = configDir + OPENALPR_CONFIG_FILE_NAME+"2";

  std::string daemonConfigFile = configDir + ALPRD_CONFIG_FILE_NAME;
  
  int recognitionThreadsCount = fileExists(openAlprConfigFile2.c_str()) ? 2 : 1;

  // Validate that the configuration files exist
  if (fileExists(openAlprConfigFile1.c_str()) == false)
  {
    std::cerr << "error, openalpr.conf file does not exist at: " << openAlprConfigFile1 << std::endl;
    return 1;
  }

  if (fileExists(daemonConfigFile.c_str()) == false)
  {
    std::cerr << "error, alprd.conf file does not exist at: " << daemonConfigFile << std::endl;
    return 1;
  }
  
  log4cplus::BasicConfigurator config;
  config.configure();
    
  if (noDaemon == false)
  {
    // Fork off into a separate daemon
    daemon(0, 0);
    
    
    log4cplus::SharedAppenderPtr myAppender(new log4cplus::RollingFileAppender(logFile));
    myAppender->setName("alprd_appender");
    // Redirect std out to log file
    logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("alprd"));
    logger.addAppender(myAppender);
    
    
    LOG4CPLUS_INFO(logger, "Running OpenALPR daemon in daemon mode.");
  }
  else
  {
    //log4cplus::SharedAppenderPtr myAppender(new log4cplus::ConsoleAppender());
    //myAppender->setName("alprd_appender");
    // Redirect std out to log file
    logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("alprd"));
    //logger.addAppender(myAppender);
    
    LOG4CPLUS_INFO(logger, "Running OpenALPR daemon in the foreground.");
  }
  
  CSimpleIniA ini;
  ini.SetMultiKey();
  
  ini.LoadFile(daemonConfigFile.c_str());
  
  std::vector<std::string> stream_urls;
  
  
  CSimpleIniA::TNamesDepend values;
  ini.GetAllValues("daemon", "stream", values);

  // sort the values into the original load order
  values.sort(CSimpleIniA::Entry::LoadOrder());

  // output all of the items
  CSimpleIniA::TNamesDepend::const_iterator i;
  for (i = values.begin(); i != values.end(); ++i) { 
      stream_urls.push_back(i->pItem);
  }
  
  
  if (stream_urls.size() == 0)
  {
    LOG4CPLUS_FATAL(logger, "No video streams defined in the configuration.");
    return 1;
  }
  
  std::string imageFolder = ini.GetValue("daemon", "store_plates_location", "/tmp/");
  bool uploadData = ini.GetBoolValue("daemon", "upload_data", false);
  std::string upload_url = ini.GetValue("daemon", "upload_address", "");

  LOG4CPLUS_INFO(logger, "Using: " << daemonConfigFile << " for daemon configuration");
  LOG4CPLUS_INFO(logger, "Using: " << imageFolder << " for storing valid plate images");
  
  pid_t pid;
  
  for (int i = 0; i < stream_urls.size(); i++)
  {
    pid = fork();
    if (pid == (pid_t) 0)
    {
      // This is the child process, kick off the capture data and upload threads
      CaptureThreadData* tdata = makeCaptureThreadDataFromIni(ini);
      tdata->stream_url = stream_urls[i];
      tdata->camera_id = i + 1;
      tdata->config_file = openAlprConfigFile;
      tdata->clock_on = clockOn;
      tdata->recognitionThreadsCount = recognitionThreadsCount;


      tthread::thread* recognition_thread;
      recognition_thread = new tthread::thread(platesRecognitionThread, (void*) tdata);
      if (fileExists(openAlprConfigFile2.c_str()))
      {
          std::cout << "start second thread;" << std::endl;
        recognition_thread = new tthread::thread(platesRecognitionThread, (void*) tdata);
      }

      usleep(10000);
      tthread::thread* thread_capture = new tthread::thread(streamCaptureThread, (void*) tdata);
      
      if (uploadData)
      {
    // Kick off the data upload thread
    UploadThreadData* udata = new UploadThreadData();
    udata->upload_url = upload_url;
    tthread::thread* thread_upload = new tthread::thread(dataUploadThread, (void*) udata );
      }
      
    }

    // Parent process will continue and spawn more children
  }



  while (daemon_active)
  {
    usleep(30000);
  }

}

unsigned char recognitionThreadsCounter = 0;
void platesRecognitionThread(void* arg)
{

    int threadNumber = recognitionThreadsCounter;
    ++recognitionThreadsCounter;

    CaptureThreadData* tdata = (CaptureThreadData*) arg;
    std::string configFile = tdata->config_file+std::to_string(threadNumber+1);
    std::cout << "Load config: " << configFile  << std::endl;
    Alpr alpr(tdata->country_code, configFile);
    alpr.setTopN(tdata->top_n);
    RecognitionData recData;
  while (daemon_active) {

      bool pop = tdata->recognitionQueues[threadNumber].pop(recData);
      if(!pop) {
          usleep(1000);
          continue;
      }

      cv::Mat &latestFrame = recData.first;
      cv::Rect roi = recData.second;

      std::vector<AlprRegionOfInterest> rois;
      rois.push_back(AlprRegionOfInterest(roi.x, roi.y, roi.width, roi.height));

      AlprResults results;
      results = alpr.recognize(latestFrame.data, latestFrame.elemSize(), latestFrame.cols, latestFrame.rows, rois);

      if (results.plates.size() > 0)
      {

        std::stringstream uuid_ss;
        uuid_ss << tdata->site_id << "-cam" << tdata->camera_id << "-" << getEpochTimeMs();
    std::string uuid = uuid_ss.str();

    // Save the image to disk (using the UUID)
    if (tdata->output_images)
    {
      std::stringstream ss;
// "-thread-" << std::to_string(threadNumber+1) <<
      ss << tdata->output_image_folder << "/" << uuid << ".jpg";

      cv::imwrite(ss.str(), latestFrame);
    }

    // Update the JSON content to include UUID and camera ID

    std::string json = alpr.toJson(results);

    cJSON *root = cJSON_Parse(json.c_str());
    cJSON_AddStringToObject(root,	"uuid",		uuid.c_str());
    cJSON_AddNumberToObject(root,	"camera_id",	tdata->camera_id);
    cJSON_AddStringToObject(root, 	"site_id", 	tdata->site_id.c_str());
    cJSON_AddNumberToObject(root,	"img_width",	latestFrame.cols);
    cJSON_AddNumberToObject(root,	"img_height",	latestFrame.rows);

        // Add the company ID to the output if configured
        if (tdata->company_id.length() > 0)
          cJSON_AddStringToObject(root, 	"company_id", 	tdata->company_id.c_str());

    char *out;
    out=cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    std::string response(out);

    free(out);

    // Push the results to the Beanstalk queue
    for (int j = 0; j < results.plates.size(); j++)
    {
      LOG4CPLUS_DEBUG(logger, "Writing plate " << results.plates[j].bestPlate.characters << " (" <<  uuid << ") to queue.");
    }

    writeToQueue(response);
      }
    }
}

void streamCaptureThread(void* arg)
{
  CaptureThreadData* tdata = (CaptureThreadData*) arg;
  
  LOG4CPLUS_INFO(logger, "country: " << tdata->country_code << " -- config file: " << tdata->config_file );
  LOG4CPLUS_INFO(logger, "Stream " << tdata->camera_id << ": " << tdata->stream_url);
  
  MotionDetector motiondetector(tdata->motion_mog_history_size, tdata->motion_mog_var_threshold, tdata->motion_mog_detect_shadows,
                                tdata->motion_debug_show_images);
  motiondetector.setErodeElementSize(tdata->motion_noise_erase_element_size);
  motiondetector.setRoi(cv::Rect(tdata->motion_roi_x, tdata->motion_roi_y, tdata->motion_roi_width, tdata->motion_roi_height));
  
  int framenum = 0;
  
  LoggingVideoBuffer videoBuffer(logger);
  
  videoBuffer.connect(tdata->stream_url, 5);
  
  cv::Mat latestFrame;
  
  std::vector<uchar> buffer;
  
  LOG4CPLUS_INFO(logger, "Starting camera " << tdata->camera_id);
  
  while (daemon_active)
  {
    std::vector<cv::Rect> regionsOfInterest;
    int response = videoBuffer.getLatestFrame(&latestFrame, regionsOfInterest);
    
    if (response != -1)
    {
      
      timespec startTime;
      getTimeMonotonic(&startTime);
      
      cv::Rect roi;
      if (tdata->detect_motion)
      {
          if (framenum == 0) motiondetector.ResetMotionDetection(&latestFrame);
          roi = motiondetector.MotionDetect(&latestFrame);
      }
      else
      {
          roi = cv::Rect(0,0, latestFrame.cols, latestFrame.rows);
      }

      if(roi.area() > 0) { // Todo: add minimum size
        tdata->recognitionQueues[0].push(RecognitionData(latestFrame.clone(), roi));
        if(tdata->recognitionThreadsCount > 1)
            tdata->recognitionQueues[1].push(RecognitionData(latestFrame.clone(), roi));
      }
    
    usleep(10000);
    framenum++;
    }
  }
  
  
  videoBuffer.disconnect();
  
  LOG4CPLUS_INFO(logger, "Video processing ended");
  
  delete tdata;
}


bool writeToQueue(std::string jsonResult)
{
  try
  {
    Beanstalk::Client client(BEANSTALK_QUEUE_HOST, BEANSTALK_PORT);
    client.use(BEANSTALK_TUBE_NAME);

    int id = client.put(jsonResult);
    
    if (id <= 0)
    {
      LOG4CPLUS_ERROR(logger, "Failed to write data to queue");
      return false;
    }
    
    LOG4CPLUS_DEBUG(logger, "put job id: " << id );

  }
  catch (const std::runtime_error& error)
  {
    LOG4CPLUS_WARN(logger, "Error connecting to Beanstalk.  Result has not been saved.");
    return false;
  }
  return true;
}



void dataUploadThread(void* arg)
{
  CURL *curl;

  
  /* In windows, this will init the winsock stuff */ 
  curl_global_init(CURL_GLOBAL_ALL);
  
  
  UploadThreadData* udata = (UploadThreadData*) arg;
  

  
  
  while(daemon_active)
  {
    try
    {
      /* get a curl handle */ 
      curl = curl_easy_init();
      Beanstalk::Client client(BEANSTALK_QUEUE_HOST, BEANSTALK_PORT);
      
      client.watch(BEANSTALK_TUBE_NAME);
    
      while (daemon_active)
      {
	Beanstalk::Job job;
	
	client.reserve(job);
	
	if (job.id() > 0)
	{
	  //LOG4CPLUS_DEBUG(logger, job.body() );
	  if (uploadPost(curl, udata->upload_url, job.body()))
	  {
	    client.del(job.id());
	    LOG4CPLUS_INFO(logger, "Job: " << job.id() << " successfully uploaded" );
	    // Wait 10ms
	    sleep_ms(10);
	  }
	  else
	  {
	    client.release(job);
	    LOG4CPLUS_WARN(logger, "Job: " << job.id() << " failed to upload.  Will retry." );
	    // Wait 2 seconds
	    sleep_ms(2000);
	  }
	}
	
      }
      
      /* always cleanup */ 
      curl_easy_cleanup(curl);
    }
    catch (const std::runtime_error& error)
    {
      LOG4CPLUS_WARN(logger, "Error connecting to Beanstalk.  Will retry." );
    }
    // wait 5 seconds
    usleep(5000000);
  }
  
  curl_global_cleanup();
}


bool uploadPost(CURL* curl, std::string url, std::string data)
{
  bool success = true;
  CURLcode res;
  struct curl_slist *headers=NULL; // init to NULL is important

  /* Add the required headers */ 
  headers = curl_slist_append(headers,  "Accept: application/json");
  headers = curl_slist_append( headers, "Content-Type: application/json");
  headers = curl_slist_append( headers, "charsets: utf-8");
 
  if(curl) {
	/* Add the headers */
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   
    /* First set the URL that is about to receive our POST. This URL can
       just as well be a https:// URL if that is what should receive the
       data. */ 
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    /* Now specify the POST data */ 
    //char* escaped_data = curl_easy_escape(curl, data.c_str(), data.length());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    //curl_free(escaped_data);
 
    /* Perform the request, res will get the return code */ 
    res = curl_easy_perform(curl);
    /* Check for errors */ 
    if(res != CURLE_OK)
    {
      success = false;
    }
 
  }
  
  return success;

  
}

