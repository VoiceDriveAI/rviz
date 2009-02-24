/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "marker_display.h"
#include "common.h"
#include "helpers/robot.h"

#include <ogre_tools/arrow.h>
#include <ogre_tools/shape.h>
#include <ogre_tools/billboard_line.h>

#include <ros/node.h>
#include <tf/transform_listener.h>
#include <tf/message_notifier.h>

#include <urdf/URDF.h>
#include <mechanism_model/robot.h>
#include <planning_models/kinematic.h>

#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>


namespace rviz
{

MarkerDisplay::MarkerDisplay( const std::string& name, VisualizationManager* manager )
: Display( name, manager )
{
  scene_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode();

  urdf_ = new robot_desc::URDF();
  descr_ = new mechanism::Robot();

  kinematic_model_ = new planning_models::KinematicModel();
  kinematic_model_->setVerbose( false );

  notifier_ = new tf::MessageNotifier<robot_msgs::VisualizationMarker>(tf_, ros_node_, boost::bind(&MarkerDisplay::incomingMarker, this, _1), "", "", 100);
}

MarkerDisplay::~MarkerDisplay()
{
  unsubscribe();

  delete notifier_;

  delete urdf_;
  delete descr_;
  delete kinematic_model_;

  clearMarkers();
}

void MarkerDisplay::clearMarkers()
{
  M_IDToMarker::iterator marker_it = markers_.begin();
  M_IDToMarker::iterator marker_end = markers_.end();
  for ( ; marker_it != marker_end; ++marker_it )
  {
    MarkerInfo& info = marker_it->second;
    delete info.object_;
  }
  markers_.clear();
}

void MarkerDisplay::onEnable()
{
  subscribe();

  scene_node_->setVisible( true );

  std::string content;
  /// @todo pass this in
  ros_node_->getParam("robotdesc/pr2", content);

  TiXmlDocument doc;
  doc.Parse(content.c_str());
  if (!doc.RootElement())
    return;

  mechanism::Robot descr;
  descr_->initXml(doc.RootElement());

  urdf_->clear();
  urdf_->loadString(content.c_str());

  kinematic_model_->build( *urdf_ );
  kinematic_model_->defaultState();
}

void MarkerDisplay::onDisable()
{
  unsubscribe();
  notifier_->clear();

  clearMarkers();

  scene_node_->setVisible( false );
}

void MarkerDisplay::subscribe()
{
  if ( !isEnabled() )
  {
    return;
  }

  notifier_->setTopic("visualizationMarker");
}

void MarkerDisplay::unsubscribe()
{
  notifier_->setTopic("");
}

void MarkerDisplay::incomingMarker( const MarkerPtr& message )
{
  boost::mutex::scoped_lock lock(queue_mutex_);

  message_queue_.push_back( message );
}

void MarkerDisplay::processMessage( const MarkerPtr& message )
{
  switch ( message->action )
  {
  case robot_msgs::VisualizationMarker::ADD:
    processAdd( message );
    break;

  case robot_msgs::VisualizationMarker::DELETE:
    processDelete( message );
    break;

  default:
    ROS_ERROR( "Unknown marker action: %d\n", message->action );
  }
}

void MarkerDisplay::processAdd( const MarkerPtr& message )
{
  ogre_tools::Object* object = NULL;
  bool create = true;

  M_IDToMarker::iterator it = markers_.find( message->id );
  if ( it != markers_.end() )
  {
    MarkerInfo& info = it->second;
    if ( message->type == info.message_->type )
    {
      object = info.object_;

      info.message_ = message;
      create = false;
    }
    else
    {
      delete it->second.object_;
      markers_.erase( it );
    }
  }

  if ( create )
  {
    switch ( message->type )
    {
    case robot_msgs::VisualizationMarker::CUBE:
      {
        ogre_tools::Shape* cube = new ogre_tools::Shape( ogre_tools::Shape::Cube, scene_manager_, scene_node_ );

        object = cube;
      }
      break;

    case robot_msgs::VisualizationMarker::CYLINDER:
      {
        ogre_tools::Shape* cylinder = new ogre_tools::Shape( ogre_tools::Shape::Cylinder, scene_manager_, scene_node_ );

        object = cylinder;
      }
      break;

    case robot_msgs::VisualizationMarker::SPHERE:
      {
        ogre_tools::Shape* sphere = new ogre_tools::Shape( ogre_tools::Shape::Sphere, scene_manager_, scene_node_ );

        object = sphere;
      }
      break;

    case robot_msgs::VisualizationMarker::ARROW:
      {
        object = new ogre_tools::Arrow( scene_manager_, scene_node_, 0.8, 0.5, 0.2, 1.0 );
      }
      break;

    case robot_msgs::VisualizationMarker::ROBOT:
      {
        Robot* robot = new Robot( scene_manager_ );
        robot->load( *descr_, false, true );
        robot->update( kinematic_model_, fixed_frame_ );

        object = robot;
      }
      break;

    case robot_msgs::VisualizationMarker::LINE_STRIP:
      {
        ogre_tools::BillboardLine* line = new ogre_tools::BillboardLine( scene_manager_, scene_node_ );
        object = line;
      }
      break;
    default:
      ROS_ERROR( "Unknown marker type: %d\n", message->type );
    }

    if ( object )
    {
      markers_.insert( std::make_pair( message->id, MarkerInfo(object, message) ) );
    }
  }

  if ( object )
  {
    setValues( message, object );

    causeRender();
  }
}

void MarkerDisplay::processDelete( const MarkerPtr& message )
{
  M_IDToMarker::iterator it = markers_.find( message->id );
  if ( it != markers_.end() )
  {
    delete it->second.object_;
    markers_.erase( it );
  }

  causeRender();
}

void MarkerDisplay::setValues( const MarkerPtr& message, ogre_tools::Object* object )
{
  std::string frame_id = message->header.frame_id;
  if ( frame_id.empty() )
  {
    frame_id = fixed_frame_;
  }

  tf::Stamped<tf::Pose> pose( btTransform( btQuaternion( message->yaw, message->pitch, message->roll ), btVector3( message->x, message->y, message->z ) ),
                              message->header.stamp, frame_id );
  try
  {
    tf_->transformPose( fixed_frame_, pose, pose );
  }
  catch(tf::TransformException& e)
  {
    ROS_ERROR( "Error transforming marker '%d' from frame '%s' to frame '%s': %s\n", message->id, frame_id.c_str(), fixed_frame_.c_str(), e.what() );
  }

  Ogre::Vector3 position( pose.getOrigin().x(), pose.getOrigin().y(), pose.getOrigin().z() );
  robotToOgre( position );

  btQuaternion quat;
  pose.getBasis().getRotation( quat );
  Ogre::Quaternion orientation;
  ogreToRobot( orientation );
  orientation = Ogre::Quaternion( quat.w(), quat.x(), quat.y(), quat.z() ) * orientation;
  robotToOgre( orientation );

  Ogre::Vector3 scale( message->xScale, message->yScale, message->zScale );
  scaleRobotToOgre( scale );

  object->setPosition( position );
  object->setOrientation( orientation );
  object->setScale( scale );
  object->setColor( message->r / 255.0f, message->g / 255.0f, message->b / 255.0f, message->alpha / 255.0f );
  object->setUserData( Ogre::Any( (void*)this ) );

  if ( message->type == robot_msgs::VisualizationMarker::LINE_STRIP )
  {
    ogre_tools::BillboardLine* line = dynamic_cast<ogre_tools::BillboardLine*>(object);
    ROS_ASSERT( line );

    line->clear();
    line->setLineWidth( message->xScale );

    std::vector<robot_msgs::Point>::iterator it = message->points.begin();
    std::vector<robot_msgs::Point>::iterator end = message->points.end();
    for ( ; it != end; ++it )
    {
      robot_msgs::Point& p = *it;

      Ogre::Vector3 v( p.x, p.y, p.z );
      robotToOgre( v );

      line->addPoint( v );
    }
  }
}

void MarkerDisplay::update( float dt )
{
  V_MarkerMessage local_queue;

  {
    boost::mutex::scoped_lock lock(queue_mutex_);

    local_queue.swap( message_queue_ );
  }

  if ( !local_queue.empty() )
  {
    V_MarkerMessage::iterator message_it = local_queue.begin();
    V_MarkerMessage::iterator message_end = local_queue.end();
    for ( ; message_it != message_end; ++message_it )
    {
      MarkerPtr& marker = *message_it;

      processMessage( marker );
    }
  }
}

void MarkerDisplay::targetFrameChanged()
{
}

void MarkerDisplay::fixedFrameChanged()
{
  notifier_->setTargetFrame( fixed_frame_ );

  clearMarkers();
}

void MarkerDisplay::reset()
{
  clearMarkers();
}

const char* MarkerDisplay::getDescription()
{
  return "Displays visualization markers sent over the visualizationMarker topic.";
}

} // namespace rviz