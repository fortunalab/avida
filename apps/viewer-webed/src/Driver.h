#ifndef Avida_WebViewer_Driver_h
#define Avida_WebViewer_Driver_h

#include "cStats.h"
#include "cWorld.h"
#include "cPopulation.h"
#include "cHardwareBase.h"
#include "cEventList.h"
#include "cUserFeedback.h"
#include "avida/core/WorldDriver.h"
#include "avida/data/Manager.h"
#include "avida/data/Package.h"
#include <iostream>
#include <sstream>

#include <emscripten.h>
#include "Messaging.h"
#include "json.hpp"


using namespace Avida;
using json = nlohmann::json;

namespace Avida{
  namespace WebViewer{
    class Driver : public WorldDriver
    {
    private:
      void Pause()  { m_paused = (m_paused) ? false : true; }
      void Finish() {m_finished = true;}
      void Abort(AbortCondition cnd) {}
      void RegisterCallback(DriverCallback callback) {}
      
    protected:
      bool m_paused;
      bool m_finished;
      cUserFeedback m_feedback;
      cWorld* m_world;
      bool m_first_update;
      cAvidaContext* m_ctx;
      
      void ProcessFeedback();
      void Setup(cWorld*, cUserFeedback);
      void ProcessAddEvent(const WebViewerMsg& msg, WebViewerMsg& ret_msg);
      bool ValidateEventMessage(const json& msg);
      bool JsonToEventFormat(json msg, string& line);
      
      
    public:
      Driver(cWorld* world, cUserFeedback feedback)
      { Driver::Setup(world, feedback); }
      ~Driver() {GlobalObjectManager::Unregister(this);}
      Driver() = delete;
      Driver(const Driver&) = delete;
      
      bool Ready() const {return (!m_finished && m_world!=nullptr && m_ctx!=nullptr);};
      bool IsFinished() const { return m_finished; }
      bool IsPaused() const { return m_paused; }
      Avida::Feedback& Feedback()  {return m_feedback;}
      
      void ProcessMessage(const WebViewerMsg& msg);
      bool StepUpdate();
      void Stop();
      json GetPopulationData();
      
    };
    
    
    void Driver::ProcessFeedback()
    {
      for (int k=0; k<m_feedback.GetNumMessages(); ++k){
        cUserFeedback::eFeedbackType msg_type = m_feedback.GetMessageType(k);
        const cString& msg = m_feedback.GetMessage(k);
        WebViewerMsg ret_msg;
        switch(msg_type){
          case cUserFeedback::eFeedbackType::UF_ERROR:
            ret_msg = FeedbackMessage(Feedback::FATAL);
            ret_msg["message"] = msg.GetData();
            break;
          case cUserFeedback::eFeedbackType::UF_WARNING:
            ret_msg = FeedbackMessage(Feedback::WARNING);
            ret_msg["message"] = msg.GetData();
            break;
          case cUserFeedback::eFeedbackType::UF_NOTIFICATION:
            ret_msg = FeedbackMessage(Feedback::NOTIFICATION);
            ret_msg["message"] = msg.GetData();
            break;
          case cUserFeedback::eFeedbackType::UF_DATA:
            ret_msg = FeedbackMessage(Feedback::DATA);
            ret_msg["message"] = nlohmann::json::parse(msg.GetData());
            break;
          default:
            ret_msg = FeedbackMessage(Feedback::UNKNOWN);
            ret_msg["message"] = msg.GetData();
            break;
        }
        PostMessage(ret_msg);
      }
      m_feedback.Clear();
    }
    
    
    void Driver::Setup(cWorld* a_world, cUserFeedback feedback)
    {
      GlobalObjectManager::Register(this);
      m_feedback = feedback;
      if (!a_world){
        m_feedback.Error("Driver is unable to find the world.");
      } else {
        // Setup our members 
        m_paused = true;
        m_finished = false;
        m_world = a_world;
        m_ctx = new cAvidaContext(this, m_world->GetRandom());
        m_world->SetDriver(this);
      }     
    }
    
    
    void Driver::ProcessMessage(const WebViewerMsg& msg)
    {
      cerr << msg.dump() << endl;
      //This message is missing it's type; can't process.
      if (msg.find("type") == msg.end()) {  
        cerr << "Message is missing type" << endl;
        WebViewerMsg error_msg = FeedbackMessage(Feedback::WARNING);
        error_msg["request"];
        PostMessage(error_msg);
      } else {
        WebViewerMsg ret_msg = ReturnMessage(msg);
        ret_msg["success"] = false;
        if (msg["type"] == "addEvent") {  //This message is requesting we add an Event
          cerr << "Message is addEvent type" << endl;
          ProcessAddEvent(msg, ret_msg);  //So try to add it.
          cerr << "Done processing message" << endl;
        }
        else {
          cerr << "Message is unknown type" << endl;
          ret_msg["message"] = "unknown type";  //We don't know what this message wants
        }
        cerr << "About to PostMessage: " << ret_msg.dump() << endl;
        PostMessage(ret_msg);
        cerr << "About to ProcessFeedback: " << endl;
        ProcessFeedback();
        cerr << "Done processing message." << endl;
      }
    }
    
    
    void Driver::ProcessAddEvent(const WebViewerMsg& rcv_msg, WebViewerMsg& ret_msg)
    {
    
      cerr << "Attempting to addEvent" << endl;
      
      //Some properties aren't required; we'll add defaults if they are missing
      WebViewerMsg msg = DefaultAddEventMessage(rcv_msg);
      
      cerr << "Defaulted message to be processed is " << msg << endl;
      
      //Validate that the properties in the addEvent message is correct.
      if (!ValidateEventMessage(msg)){
        cerr << "addEvent is invalid" << endl;
        ret_msg["success"] = false;
      }
      //TODO: actually make run pause action; for now just treat it as immediate
      //Right now any addEvent with runPause will happen immediately.
      else if (msg["name"] == "runPause"){
        cerr << "Event is RunPause" << endl;
        ret_msg["success"] = true;
        Pause();
      } else {
        cerr << "Event is other than runPause" << endl;
        string event_line;  //This will contain a properly formatted event list line if successful
        if (!JsonToEventFormat(msg, event_line)){
          //Because we are avoiding exceptions, we're using a bool to flag success
          //If we're here, we were unsuccessful and need to send feedback and post
          //a failure response message
          ret_msg["message"] = "Missing properties; unable to addEvent";
        } 
        else {
          //We were able to create a line from an event file, now let's try to add it
          //to the event list; if we can't, feedback will be generated and success
          //will be set to false.
          cerr << "Trying to add event line: " << event_line << endl;
          ret_msg["success"] = m_world->GetEventsList()->AddEventFileFormat(event_line.data(), m_feedback);
        }
      } //Done with non runPause message processing
    }
    
    bool Driver::ValidateEventMessage(const json& msg)
    {
      bool success = true;
      
      //Action name is missing
      if (msg.find("name") == msg.end()){
        success=false;
        m_feedback.Warning("addEvent is missing name property");
      }
      // Can't find event trigger type
      if (msg.find("triggerType") == msg.end()){
        success=false;
        m_feedback.Warning("addEvent is missing triggerType property");
      }
      //Missing start condition
      if (msg.find("start") == msg.end()){
        success = false;
        m_feedback.Warning("addEvent is missing start property");
      }
      // Can't find the event interval
      if (msg.find("interval") == msg.end()){
        success=false;
        m_feedback.Warning("addEvent is missing interval property");
      }
      // Can't find the event end 
      if (msg.find("end") == msg.end() || msg["end"] != ""){
        success=false;
        m_feedback.Warning("addEvent is missing end property");
      }
      return success;
    }
    
    
    
    bool Driver::JsonToEventFormat(json msg, string& line)
    {
      ostringstream line_in;
             
      line_in << DeQuote(msg["triggerType"]) << " ";
      line_in << DeQuote(msg["start"]) << ":" << DeQuote(msg["interval"]);
      if (msg["end"] != "")
        line_in << ":" << DeQuote(msg["end"]);
      line_in << " " << DeQuote(msg["name"]);
      if (msg.find("args") != msg.end())
        for (auto arg : msg["args"]) // copy each array element
          line_in << " " << DeQuote(arg);  // to the input line
      line = line_in.str();  //Return our input from the stream
      return true;
    }
    
    
    bool Driver::StepUpdate()
    {
      if (m_paused || m_finished)
        return false;
      cPopulation& population = m_world->GetPopulation();
      cStats& stats = m_world->GetStats();
      
      const int ave_time_slice = m_world->GetConfig().AVE_TIME_SLICE.Get();
      const double point_mut_prob = m_world->GetConfig().POINT_MUT_PROB.Get() +
      m_world->GetConfig().POINT_INS_PROB.Get() +
      m_world->GetConfig().POINT_DEL_PROB.Get();
      
      //Perform a single update
      ProcessFeedback(); 
      
      m_world->GetEvents(*m_ctx);
      if (m_finished == true) return false;
      
      // Increment the Update.
      stats.IncCurrentUpdate();
      
      std::cerr << "Update: " << stats.GetUpdate() << std::endl;
      
      population.ProcessPreUpdate();
      
      // Handle all data collection for previous update.
      if (stats.GetUpdate() > 0) {
        // Tell the stats object to do update calculations and printing.
        stats.ProcessUpdate();
      }
      
      
      // Process the update.
      const int UD_size = ave_time_slice * population.GetNumOrganisms();
      const double step_size = 1.0 / (double) UD_size;
      
      for (int i = 0; i < UD_size; i++) 
        population.ProcessStep(*m_ctx, step_size, population.ScheduleOrganism());
      
      // end of update stats...
      population.ProcessPostUpdate(*m_ctx);
      
      
      // Do Point Mutations
      if (point_mut_prob > 0 ) {
        for (int i = 0; i < population.GetSize(); i++) {
          if (population.GetCell(i).IsOccupied()) {
            population.GetCell(i).GetOrganism()->GetHardware().PointMutate(*m_ctx);
          }
        }
      }
      
      ProcessFeedback();
      
      // Exit conditons...
      if (population.GetNumOrganisms() == 0) m_finished = true;
      return true;
    } //Driver.h
  } //WebViewer namespace
} //Avida namespace



#endif