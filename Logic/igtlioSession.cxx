#include "igtlioSession.h"

#include <vtkObjectFactory.h>
#include <vtkNew.h>

#include "igtlioConnector.h"
#include <vtksys/SystemTools.hxx>
#include "vtkTimerLog.h"
#include "igtlioCommandDevice.h"
#include "igtlioImageDevice.h"
#include "igtlioTransformDevice.h"


namespace igtlio
{

//---------------------------------------------------------------------------
vtkStandardNewMacro(vtkIGTLIOSession);


//----------------------------------------------------------------------

void vtkIGTLIOSession::PrintSelf(std::ostream &, vtkIndent)
{

}

vtkIGTLIOSession::vtkIGTLIOSession()
{
}

DevicePointer vtkIGTLIOSession::AddDeviceIfNotPresent(DeviceKeyType key)
{
  DevicePointer device = Connector->GetDevice(key);

  if (!device)
  {
    device = Connector->GetDeviceFactory()->create(key.type, key.name);
    Connector->AddDevice(device);
  }

  return device;
}


CommandDevicePointer vtkIGTLIOSession::SendCommandQuery(std::string device_id,
                                                                 std::string command,
                                                                 std::string content,
                                                                 igtlio::SYNCHRONIZATION_TYPE synchronized,
                                                                 double timeout_s)
{
  vtkSmartPointer<CommandDevice> device;
  DeviceKeyType key(igtlio::CommandConverter::GetIGTLTypeName(), device_id);
  device = CommandDevice::SafeDownCast(this->AddDeviceIfNotPresent(key));

  igtlio::CommandConverter::ContentData contentdata = device->GetContent();
  contentdata.id +=1;
  contentdata.name = command;
  contentdata.content = content;
  device->SetContent(contentdata);

  device->PruneCompletedQueries();

  Connector->SendMessage(CreateDeviceKey(device));

  if (synchronized==igtlio::BLOCKING)
  {
    double starttime = vtkTimerLog::GetUniversalTime();
    while (vtkTimerLog::GetUniversalTime() - starttime < timeout_s)
    {
      Connector->PeriodicProcess();
      vtksys::SystemTools::Delay(5);

      CommandDevicePointer response = device->GetResponseFromCommandID(contentdata.id);

      if (response)
      {
        return response;
      }
    }
  }
  else
  {
    return device;
  }

  return vtkSmartPointer<CommandDevice>();
}

CommandDevicePointer vtkIGTLIOSession::SendCommandResponse(std::string device_id, std::string command, std::string content)
{
  DeviceKeyType key(igtlio::CommandConverter::GetIGTLTypeName(), device_id);
  CommandDevicePointer device = CommandDevice::SafeDownCast(Connector->GetDevice(key));

  igtlio::CommandConverter::ContentData contentdata = device->GetContent();

  if (command != contentdata.name)
  {
    vtkErrorMacro("Requested command response " << command << " does not match the existing query: " << contentdata.name);
    return CommandDevicePointer();
  }

  contentdata.name = command;
  contentdata.content = content;
  device->SetContent(contentdata);

  Connector->SendMessage(CreateDeviceKey(device), Device::MESSAGE_PREFIX_REPLY);
  return device;
}

ImageDevicePointer vtkIGTLIOSession::SendImage(std::string device_id, vtkSmartPointer<vtkImageData> image, vtkSmartPointer<vtkMatrix4x4> transform)
{
  ImageDevicePointer device;
  DeviceKeyType key(igtlio::ImageConverter::GetIGTLTypeName(), device_id);
  device = ImageDevice::SafeDownCast(this->AddDeviceIfNotPresent(key));

  igtlio::ImageConverter::ContentData contentdata = device->GetContent();
  contentdata.image = image;
  contentdata.transform = transform;
  device->SetContent(contentdata);

  Connector->SendMessage(CreateDeviceKey(device));

  return device;
}

ConnectorPointer vtkIGTLIOSession::GetConnector()
{
  return Connector;
}

void vtkIGTLIOSession::SetConnector(ConnectorPointer connector)
{
  Connector = connector;
}

void vtkIGTLIOSession::StartServer(int serverPort, igtlio::SYNCHRONIZATION_TYPE sync, double timeout_s)
{
  if (!Connector)
  {
    vtkWarningMacro("No connector, ignoring connect request");
    return;
  }
  if (Connector->GetState()!=Connector::STATE_OFF)
  {
    vtkWarningMacro("Session is already working, ignoring connect request");
    return;
  }

  if (serverPort<=0)
    serverPort = Connector->GetServerPort();

  Connector->SetTypeServer(serverPort);
  Connector->Start();

  if (sync==igtlio::BLOCKING)
  {
    this->waitForConnection(timeout_s);
  }
}

void vtkIGTLIOSession::ConnectToServer(std::string serverHost, int serverPort, igtlio::SYNCHRONIZATION_TYPE sync, double timeout_s)
{
  if (!Connector)
  {
    vtkWarningMacro("No connector, ignoring connect request");
    return;
  }
  if (Connector->GetState()!=Connector::STATE_OFF)
  {
    vtkWarningMacro("Session is already working, ignoring connect request");
    return;
  }

  if (serverPort<=0)
    serverPort = Connector->GetServerPort();
  Connector->SetTypeClient(serverHost, serverPort);
  Connector->Start();

  if (sync==igtlio::BLOCKING)
  {
    this->waitForConnection(timeout_s);
  }
}

bool vtkIGTLIOSession::waitForConnection(double timeout_s)
{
  double starttime = vtkTimerLog::GetUniversalTime();

  while (vtkTimerLog::GetUniversalTime() - starttime < timeout_s)
  {
    Connector->PeriodicProcess();
    vtksys::SystemTools::Delay(5);

    if (Connector->GetState() != Connector::STATE_WAIT_CONNECTION)
    {
      break;
    }
  }

  return Connector->GetState() == Connector::STATE_CONNECTED;
}


TransformDevicePointer vtkIGTLIOSession::SendTransform(std::string device_id, vtkSmartPointer<vtkMatrix4x4> transform)
{
  TransformDevicePointer device;
  DeviceKeyType key(igtlio::TransformConverter::GetIGTLTypeName(), device_id);
  device = TransformDevice::SafeDownCast(this->AddDeviceIfNotPresent(key));

  igtlio::TransformConverter::ContentData contentdata = device->GetContent();
  contentdata.deviceName = device_id;
  contentdata.transform = transform;
  device->SetContent(contentdata);

  Connector->SendMessage(CreateDeviceKey(device));

  return device;
}

} //namespace igtlio
