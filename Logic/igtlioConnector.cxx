/*=auto=========================================================================

Portions (c) Copyright 2009 Brigham and Women's Hospital (BWH) All Rights Reserved.

See Doc/copyright/copyright.txt
or http://www.slicer.org/copyright/copyright.txt for details.

Program:   3D Slicer
Module:    $RCSfile: vtkMRMLGradientAnisotropicDiffusionFilterNode.cxx,v $
Date:      $Date: 2006/03/17 15:10:10 $
Version:   $Revision: 1.2 $

=========================================================================auto=*/

// OpenIGTLinkIF MRML includes
#include "igtlioConnector.h"

#include <igtl_header.h>
#include <igtlServerSocket.h>
#include <igtlClientSocket.h>
#include <igtlOSUtil.h>
#include <igtlMessageBase.h>
#include <igtlMessageHeader.h>

// MRML includes
//#include <vtkMRMLScene.h>

// VTK includes
#include <vtkCommand.h>
#include <vtkCollection.h>
//#include <vtkEventBroker.h>
#include <vtkImageData.h>
#include <vtkIntArray.h>
#include <vtkMultiThreader.h>
#include <vtkMutexLock.h>
#include <vtkObjectFactory.h>
#include <vtkTimerLog.h>

// STD includes
#include <string>
#include <iostream>
#include <sstream>
#include <map>
#include "igtlioCircularBuffer.h"

namespace igtlio
{

std::string DeviceKeyType::GetBaseTypeName() const
{
  int pos = type.find("_");
  if (pos>=0)
    return type.substr(pos+1);
  return type;
}

DeviceKeyType CreateDeviceKey(igtl::MessageBase::Pointer message)
{
  if (!message)
    return DeviceKeyType();
  return DeviceKeyType(message->GetDeviceType(), message->GetDeviceName());
}

DeviceKeyType CreateDeviceKey(DevicePointer device)
{
  if (!device)
    return DeviceKeyType();
  return DeviceKeyType(device->GetDeviceType(), device->GetDeviceName());
}


bool operator<(const DeviceKeyType &lhs, const DeviceKeyType &rhs)
{
  if (lhs.GetBaseTypeName() > rhs.GetBaseTypeName())
      return false;
  if (lhs.GetBaseTypeName() < rhs.GetBaseTypeName())
      return true;
  return (lhs.name < rhs.name);
}

bool operator==(const DeviceKeyType &lhs, const DeviceKeyType &rhs)
{
  return (lhs.GetBaseTypeName()==rhs.GetBaseTypeName()) &&
      (lhs.name==rhs.name);
}




//------------------------------------------------------------------------------
vtkStandardNewMacro(Connector);

//----------------------------------------------------------------------------
const char *Connector::ConnectorTypeStr[Connector::NUM_TYPE] =
{
  "?", // TYPE_NOT_DEFINED
  "S", // TYPE_SERVER
  "C", // TYPE_CLIENT
};

//----------------------------------------------------------------------------
const char *Connector::ConnectorStateStr[Connector::NUM_STATE] =
{
  "OFF",       // OFF
  "WAIT",      // WAIT_CONNECTION
  "ON",        // CONNECTED
};

//----------------------------------------------------------------------------
Connector::Connector()
{
  this->Type   = TYPE_CLIENT;
  this->State  = STATE_OFF;
  this->Persistent = PERSISTENT_OFF;

  this->Thread = vtkMultiThreaderPointer::New();
  this->ServerStopFlag = false;
  this->ThreadID = -1;
  this->ServerHostname = "localhost";
  this->ServerPort = 18944;
  this->Mutex = vtkMutexLockPointer::New();
  this->CircularBufferMutex = vtkMutexLockPointer::New();
  this->RestrictDeviceName = 0;

  this->EventQueueMutex = vtkMutexLockPointer::New();

  this->PushOutgoingMessageFlag = 0;
  this->PushOutgoingMessageMutex = vtkMutexLockPointer::New();

  this->CheckCRC = 1;

  DeviceFactory = DeviceFactoryPointer::New();
}

//----------------------------------------------------------------------------
Connector::~Connector()
{
  this->Stop();
}

void Connector::PrintSelf(ostream& os, vtkIndent indent)
{
  Superclass::PrintSelf(os,indent);

  if (this->Type == TYPE_SERVER)
    {
    os << indent << "Connector Type : SERVER\n";
    os << indent << "Listening Port #: " << this->ServerPort << "\n";
    }
  else if (this->Type == TYPE_CLIENT)
    {
    os << indent << "Connector Type: CLIENT\n";
    os << indent << "Server Hostname: " << this->ServerHostname << "\n";
    os << indent << "Server Port #: " << this->ServerPort << "\n";
    }

  switch (this->State)
    {
    case STATE_OFF:
      os << indent << "State: OFF\n";
      break;
    case STATE_WAIT_CONNECTION:
      os << indent << "State: WAIT FOR CONNECTION\n";
      break;
    case STATE_CONNECTED:
      os << indent << "State: CONNECTED\n";
      break;
    }
  os << indent << "Persistent: " << this->Persistent << "\n";
  os << indent << "Restrict Device Name: " << this->RestrictDeviceName << "\n";
  os << indent << "Push Outgoing Message Flag: " << this->PushOutgoingMessageFlag << "\n";
  os << indent << "Check CRC: " << this->CheckCRC << "\n";
  os << indent << "Number of devices: " << this->GetNumberOfDevices() << "\n";
}

//----------------------------------------------------------------------------
const char* Connector::GetServerHostname()
{
  return this->ServerHostname.c_str();
}

//----------------------------------------------------------------------------
void Connector::SetServerHostname(std::string str)
{
  if (this->ServerHostname.compare(str) == 0)
    {
    return;
    }
  this->ServerHostname = str;
  this->Modified();
}

//----------------------------------------------------------------------------
int Connector::SetTypeServer(int port)
{
  if (this->Type == TYPE_SERVER
      && this->ServerPort == port)
    {
    return 1;
    }
  this->Type = TYPE_SERVER;
  this->ServerPort = port;
  this->Modified();
  return 1;
}

//----------------------------------------------------------------------------
int Connector::SetTypeClient(std::string hostname, int port)
{
  if (this->Type == TYPE_CLIENT
      && this->ServerPort == port
      && this->ServerHostname.compare(hostname) == 0)
    {
    return 1;
    }
  this->Type = TYPE_CLIENT;
  this->ServerPort = port;
  this->ServerHostname = hostname;
  this->Modified();
  return 1;
}

//----------------------------------------------------------------------------
void Connector::SetCheckCRC(bool c)
{
  this->CheckCRC = c;
}

//---------------------------------------------------------------------------
int Connector::Start()
{
  // Check if type is defined.
  if (this->Type == Connector::TYPE_NOT_DEFINED)
    {
      //vtkErrorMacro("Connector type is not defined.");
    return 0;
    }

  // Check if thread is detached
  if (this->ThreadID >= 0)
    {
      //vtkErrorMacro("Thread exists.");
    return 0;
    }

  this->ServerStopFlag = false;
  this->ThreadID = this->Thread->SpawnThread((vtkThreadFunctionType) &Connector::ThreadFunction, this);

  // Following line is necessary in some Linux environment,
  // since it takes for a while for the thread to update
  // this->State to non STATE_OFF value. This causes error
  // after calling vtkMRMLIGTLConnectorNode::Start() in ProcessGUIEvent()
  // in vtkOpenIGTLinkIFGUI class.
  this->State = STATE_WAIT_CONNECTION;
  this->InvokeEvent(Connector::ActivatedEvent);

  return 1;
}

//---------------------------------------------------------------------------
int Connector::Stop()
{
  // Check if thread exists
  if (this->ThreadID >= 0)
    {
    // NOTE: Thread should be killed by activating ServerStopFlag.
    this->ServerStopFlag = true;
    this->Mutex->Lock();
    if (this->Socket.IsNotNull())
      {
      this->Socket->CloseSocket();
      }
    this->Mutex->Unlock();
    this->Thread->TerminateThread(this->ThreadID);
    this->ThreadID = -1;
    return 1;
    }
  else
    {
    return 0;
    }
}


//---------------------------------------------------------------------------
void* Connector::ThreadFunction(void* ptr)
{
  vtkMultiThreader::ThreadInfo* vinfo =
    static_cast<vtkMultiThreader::ThreadInfo*>(ptr);
  Connector* igtlcon = static_cast<Connector*>(vinfo->UserData);

  igtlcon->State = STATE_WAIT_CONNECTION;

  if (igtlcon->Type == TYPE_SERVER)
    {
    igtlcon->ServerSocket = igtl::ServerSocket::New();
    if (igtlcon->ServerSocket->CreateServer(igtlcon->ServerPort) == -1)
      {
      vtkErrorWithObjectMacro(igtlcon, "Failed to create server socket !");
      igtlcon->ServerStopFlag = true;
      }
    }

  // Communication -- common to both Server and Client
  while (!igtlcon->ServerStopFlag)
    {
    //vtkErrorMacro("vtkOpenIGTLinkIFLogic::ThreadFunction(): alive.");
    igtlcon->Mutex->Lock();
    //igtlcon->Socket = igtlcon->WaitForConnection();
    igtlcon->WaitForConnection();
    igtlcon->Mutex->Unlock();
    if (igtlcon->Socket.IsNotNull() && igtlcon->Socket->GetConnected())
      {
      igtlcon->State = STATE_CONNECTED;
      // need to Request the InvokeEvent, because we are not on the main thread now
      igtlcon->RequestInvokeEvent(Connector::ConnectedEvent);
      //vtkErrorMacro("vtkOpenIGTLinkIFLogic::ThreadFunction(): Client Connected.");
      igtlcon->RequestPushOutgoingMessages();
      igtlcon->ReceiveController();
      igtlcon->State = STATE_WAIT_CONNECTION;
      igtlcon->RequestInvokeEvent(Connector::DisconnectedEvent); // need to Request the InvokeEvent, because we are not on the main thread now
      }
    }

  if (igtlcon->Socket.IsNotNull())
    {
    igtlcon->Socket->CloseSocket();
    }

  if (igtlcon->Type == TYPE_SERVER && igtlcon->ServerSocket.IsNotNull())
    {
    igtlcon->ServerSocket->CloseSocket();
    }

  igtlcon->ThreadID = -1;
  igtlcon->State = STATE_OFF;
  igtlcon->RequestInvokeEvent(Connector::DeactivatedEvent); // need to Request the InvokeEvent, because we are not on the main thread now

  return NULL; //why???
}


//----------------------------------------------------------------------------
void Connector::RequestInvokeEvent(unsigned long eventId)
{
  this->EventQueueMutex->Lock();
  this->EventQueue.push_back(eventId);
  this->EventQueueMutex->Unlock();
}


//----------------------------------------------------------------------------
void Connector::RequestPushOutgoingMessages()
{
  this->PushOutgoingMessageMutex->Lock();
  this->PushOutgoingMessageFlag = 1;
  this->PushOutgoingMessageMutex->Unlock();
}


//----------------------------------------------------------------------------
int Connector::WaitForConnection()
{
  //igtl::ClientSocket::Pointer socket;

  if (this->Type == TYPE_CLIENT)
    {
    //socket = igtl::ClientSocket::New();
    this->Socket = igtl::ClientSocket::New();
    }

  while (!this->ServerStopFlag)
    {
    if (this->Type == TYPE_SERVER)
      {
      //vtkErrorMacro("vtkMRMLIGTLConnectorNode: Waiting for client @ port #" << this->ServerPort);
      this->Socket = this->ServerSocket->WaitForConnection(1000);
      if (this->Socket.IsNotNull()) // if client connected
        {
        //vtkErrorMacro("vtkMRMLIGTLConnectorNode: connected.");
        return 1;
        }
      }
    else if (this->Type == TYPE_CLIENT) // if this->Type == TYPE_CLIENT
      {
      //vtkErrorMacro("vtkMRMLIGTLConnectorNode: Connecting to server...");
      int r = this->Socket->ConnectToServer(this->ServerHostname.c_str(), this->ServerPort);
      if (r == 0) // if connected to server
        {
        return 1;
        }
      else
        {
        igtl::Sleep(100);
        break;
        }
      }
    else
      {
      this->ServerStopFlag = true;
      }
    }

  if (this->Socket.IsNotNull())
    {
    //vtkErrorMacro("vtkOpenIGTLinkLogic::WaitForConnection(): Socket Closed.");
    this->Socket->CloseSocket();
    }

  //return NULL;
  return 0;
}


//----------------------------------------------------------------------------
int Connector::ReceiveController()
{
  //igtl_header header;
  igtl::MessageHeader::Pointer headerMsg;
  headerMsg = igtl::MessageHeader::New();

  if (this->Socket.IsNull())
    {
    return 0;
    }

  while (!this->ServerStopFlag)
    {
    // check if connection is alive
    if (!this->Socket->GetConnected())
      {
      break;
      }

    //----------------------------------------------------------------
    // Receive Header
    headerMsg->InitPack();


    vtkDebugMacro("Waiting for header of size: " << headerMsg->GetPackSize());

    int r = this->Socket->Receive(headerMsg->GetPackPointer(), headerMsg->GetPackSize());

    vtkDebugMacro("Received header of size: " << headerMsg->GetPackSize());


    if (r != headerMsg->GetPackSize())
      {
      //vtkErrorMacro("Irregluar size.");
      //vtkErrorMacro("Irregluar size " << r << " expecting " << headerMsg->GetPackSize() );
      vtkDebugMacro("ignoring header, breaking. received=" << r);
      break;
      }

    // Deserialize the header
    headerMsg->Unpack();

    //----------------------------------------------------------------
    // Check Device Name
    // Nov 16, 2010: Currently the following code only checks
    // if the device name is defined in the message.
    const char* devName = headerMsg->GetDeviceName();
    if (devName[0] == '\0')
      {
      /// Dec 7, 2010: Removing the following code, since message without
      /// device name should be handled in the MRML scene as well.
      //// If no device name is defined, skip processing the message.
      //this->Skip(headerMsg->GetBodySizeToRead());
      //continue; //  while (!this->ServerStopFlag)
      }
    //----------------------------------------------------------------
    // If device name is restricted
    else if (this->RestrictDeviceName)
      {
      // Check if the node has already been registered.
        //TODO: Cannot call GetDevice in Thread!!!!
        DeviceKeyType key = CreateDeviceKey(headerMsg);
      int registered = this->GetDevice(key).GetPointer() != NULL;
      if (registered == 0)
        {
        this->Skip(headerMsg->GetBodySizeToRead());
        continue; //  while (!this->ServerStopFlag)
        }
      }

    vtkDebugMacro("completed read header : " << headerMsg->GetDeviceName() << " body size to read: " << headerMsg->GetBodySizeToRead());


    //----------------------------------------------------------------
    // Search Circular Buffer
    DeviceKeyType key = CreateDeviceKey(headerMsg);

    CircularBufferMap::iterator iter = this->Buffer.find(key);
    if (iter == this->Buffer.end()) // First time to refer the device name
      {
      this->CircularBufferMutex->Lock();
      this->Buffer[key] = CircularBufferPointer::New();
      this->CircularBufferMutex->Unlock();
      }

    //----------------------------------------------------------------
    // Load to the circular buffer

    CircularBufferPointer circBuffer = this->Buffer[key];

    if (circBuffer && circBuffer->StartPush() != -1)
      {
      //std::cerr << "Pushing into the circular buffer." << std::endl;
      circBuffer->StartPush();

      igtl::MessageBase::Pointer buffer = circBuffer->GetPushBuffer();
      buffer->SetMessageHeader(headerMsg);
      buffer->AllocatePack();

      vtkDebugMacro("Waiting to receive body:  size=" << buffer->GetPackBodySize()
                    << ", GetBodySizeToRead=" << buffer->GetBodySizeToRead()
                    << ", GetPackSize=" << buffer->GetPackSize());
      int read = this->Socket->Receive(buffer->GetPackBodyPointer(), buffer->GetPackBodySize());
      vtkDebugMacro("Received body: " << read);
      if (read != buffer->GetPackBodySize())
        {
        vtkErrorMacro ("Only read " << read << " but expected to read "
                       << buffer->GetPackBodySize() << "\n");
        continue;
        }

      circBuffer->EndPush();

      }
    else
      {
      break;
      }

    } // while (!this->ServerStopFlag)

  this->Socket->CloseSocket();

  return 0;

}


//----------------------------------------------------------------------------
int Connector::SendData(int size, unsigned char* data)
{

  if (this->Socket.IsNull())
    {
    return 0;
    }

  // check if connection is alive
  if (!this->Socket->GetConnected())
    {
    return 0;
    }

  return this->Socket->Send(data, size);  // return 1 on success, otherwise 0.

}


//----------------------------------------------------------------------------
int Connector::Skip(int length, int skipFully)
{
  unsigned char dummy[256];
  int block  = 256;
  int n      = 0;
  int remain = length;

  do
    {
    if (remain < block)
      {
      block = remain;
      }

    n = this->Socket->Receive(dummy, block, skipFully);
    remain -= n;
    }
  while (remain > 0 || (skipFully && n < block));

  return (length - remain);
}


//----------------------------------------------------------------------------
unsigned int Connector::GetUpdatedBuffersList(NameListType& nameList)
{
  nameList.clear();

  CircularBufferMap::iterator iter;
  for (iter = this->Buffer.begin(); iter != this->Buffer.end(); iter ++)
    {
    if (iter->second != NULL && iter->second->IsUpdated())
      {
      nameList.push_back(iter->first);
      }
    }
  return nameList.size();
}


//----------------------------------------------------------------------------
CircularBufferPointer Connector::GetCircularBuffer(const DeviceKeyType &key)
{
  CircularBufferMap::iterator iter = this->Buffer.find(key);
  if (iter != this->Buffer.end())
    {
    return this->Buffer[key]; // the key has been found in the list
    }
  else
    {
    return NULL;  // nothing found
    }
}


//---------------------------------------------------------------------------
void Connector::ImportDataFromCircularBuffer()
{
  Connector::NameListType nameList;
  this->GetUpdatedBuffersList(nameList);

  Connector::NameListType::iterator nameIter;
  for (nameIter = nameList.begin(); nameIter != nameList.end(); nameIter ++)
    {
    DeviceKeyType key = *nameIter;
    CircularBuffer* circBuffer = this->GetCircularBuffer(key);
    circBuffer->StartPull();

    igtl::MessageBase::Pointer buffer = circBuffer->GetPullBuffer();

    vtkSmartPointer<DeviceCreator> deviceCreator = DeviceFactory->GetCreator(key.GetBaseTypeName());

    if (!deviceCreator)
      {
      vtkErrorMacro(<< "Received unknown device type " << buffer->GetDeviceType() << ", device=" << buffer->GetDeviceName());
      continue;
      }

    DevicePointer device = this->GetDevice(key);

    if ((device.GetPointer()!=NULL) && !(CreateDeviceKey(device)==CreateDeviceKey(buffer)))
      {
        vtkErrorMacro(
            << "Received an IGTL message of the wrong type, device=" << key.name
            << " has type " << device->GetDeviceType()
            << " got type " << buffer->GetDeviceType()
              );
        continue;
      }

    if (!device && !this->RestrictDeviceName)
      {
        device = deviceCreator->Create(key.name);
        device->SetMessageDirection(Device::MESSAGE_DIRECTION_IN);
        this->AddDevice(device);
      }

    device->ReceiveIGTLMessage(buffer, this->CheckCRC);
    device->Modified();
    this->InvokeEvent(Connector::DeviceModifiedEvent, device.GetPointer());

    circBuffer->EndPull();
    }

  for (unsigned int i=0; i<Devices.size(); ++i)
    {
    Devices[i]->CheckQueryExpiration();
    }
}

//---------------------------------------------------------------------------
void Connector::ImportEventsFromEventBuffer()
{
  // Invoke all events in the EventQueue

  bool emptyQueue=true;
  unsigned long eventId=0;
  do
  {
    emptyQueue=true;
    this->EventQueueMutex->Lock();
    if (this->EventQueue.size()>0)
    {
      eventId=this->EventQueue.front();
      this->EventQueue.pop_front();
      emptyQueue=false;
    }
    this->EventQueueMutex->Unlock();

    // Invoke the event
    this->InvokeEvent(eventId);

  } while (!emptyQueue);
}

//---------------------------------------------------------------------------
void Connector::PushOutgoingMessages()
{
  int push = 0;

  // Read PushOutgoingMessageFlag and reset it.
  this->PushOutgoingMessageMutex->Lock();
  push = this->PushOutgoingMessageFlag;
  this->PushOutgoingMessageFlag = 0;
  this->PushOutgoingMessageMutex->Unlock();

  if (push)
    {
      for (unsigned i=0; i<Devices.size(); ++i)
        {
          if (Devices[i]->MessageDirectionIsOut() && Devices[i]->GetPushOnConnect())
            this->PushNode(Devices[i]);
        }
    }
}

//----------------------------------------------------------------------------
void Connector::PeriodicProcess()
{
  this->ImportDataFromCircularBuffer();
  this->ImportEventsFromEventBuffer();
  this->PushOutgoingMessages();
}

int Connector::AddDevice(DevicePointer device)
{
  if (this->GetDevice(CreateDeviceKey(device))!=NULL)
    {
    vtkErrorMacro("Failed to add igtl device: " << device->GetDeviceName() << " already present");
    return 0;
    }

  device->SetTimestamp(vtkTimerLog::GetUniversalTime());
  Devices.push_back(device);
  //TODO: listen to device events?
  this->InvokeEvent(Connector::NewDeviceEvent, device.GetPointer());
  return 1;
}
  
int Connector::RemoveDevice(DevicePointer device)
{
  DeviceKeyType key = CreateDeviceKey(device);
  for (unsigned i=0; i<Devices.size(); ++i)
  {
    if (CreateDeviceKey(Devices[i])==key)
    {
      Devices.erase(Devices.begin()+i);
      this->InvokeEvent(Connector::RemovedDeviceEvent, device.GetPointer());
      return 1;
    }
  }
  vtkErrorMacro("Failed to remove igtl device: " << device->GetDeviceName());
  return 0;
}

//---------------------------------------------------------------------------
unsigned int Connector::GetNumberOfDevices() const
{
  return Devices.size();
}

//---------------------------------------------------------------------------
void Connector::RemoveDevice(int index)
{
  //TODO: disconnect listen to device events?
  DevicePointer device = Devices[index]; // ensure object lives until event has completed
  Devices.erase(Devices.begin()+index);
  this->InvokeEvent(Connector::RemovedDeviceEvent, device.GetPointer());
}

//---------------------------------------------------------------------------
DevicePointer Connector::GetDevice(int index)
{
  return Devices[index];
}

//---------------------------------------------------------------------------
DevicePointer Connector::GetDevice(DeviceKeyType key)
{
  for (unsigned i=0; i<Devices.size(); ++i)
    if (CreateDeviceKey(Devices[i])==key)
      return Devices[i];
  return DevicePointer();
}

//---------------------------------------------------------------------------
int Connector::SendMessage(DeviceKeyType device_id, Device::MESSAGE_PREFIX prefix)
{
  DevicePointer device = this->GetDevice(device_id);
  if (!device)
    {
      vtkErrorMacro("Sending OpenIGTLinkMessage: " << device_id.type << "/" << device_id.name<< ", device not found");
      return 1;
    }

  //TODO replace prefix with message-type or similar - giving the basic message same status as the queries
  igtl::MessageBase::Pointer msg = device->GetIGTLMessage(prefix);

  if (!msg)
    {
      vtkErrorMacro("Sending OpenIGTLinkMessage: " << device_id.type << "/" << device_id.name << ", message not available from device");
      return 1;
    }

  int r = this->SendData(msg->GetPackSize(), (unsigned char*)msg->GetPackPointer());
  if (r == 0)
    {
      vtkDebugMacro("Sending OpenIGTLinkMessage: " << device_id.type << "/" << device_id.name << " failed.");
      return 0;
    }
  return r;

//TODO: push the device_id Device to igtl,
// IF prefixed, i.e. send a query, also add to the query queue.
//
//  return 0;
}

DeviceFactoryPointer Connector::GetDeviceFactory()
{
  return DeviceFactory;
}

void Connector::SetDeviceFactory(DeviceFactoryPointer val)
{
  if (val==DeviceFactory)
    return;
  DeviceFactory = val;
  this->Modified();
}

//---------------------------------------------------------------------------
int Connector::PushNode(DevicePointer node, int event)
{
  // TODO: verify that removed event argument is OK
  return this->SendMessage(CreateDeviceKey(node), Device::MESSAGE_PREFIX_NOT_DEFINED);
}

} // namespace igtlio
