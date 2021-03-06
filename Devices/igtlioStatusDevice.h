#ifndef IGTLIOSTATUSDEVICE_H
#define IGTLIOSTATUSDEVICE_H

#include "igtlioDevicesExport.h"

#include "igtlioStatusConverter.h"
#include "igtlioDevice.h"

namespace igtlio
{

class vtkImageData;
typedef vtkSmartPointer<class StatusDevice> StatusDevicePointer;

/// A Device supporting the STATUS igtl Message.
class OPENIGTLINKIO_DEVICES_EXPORT StatusDevice : public Device
{
public:
 virtual std::string GetDeviceType() const;
 virtual int ReceiveIGTLMessage(igtl::MessageBase::Pointer buffer, bool checkCRC);
 virtual igtl::MessageBase::Pointer GetIGTLMessage();
 virtual igtl::MessageBase::Pointer GetIGTLMessage(MESSAGE_PREFIX prefix);
 virtual std::set<MESSAGE_PREFIX> GetSupportedMessagePrefixes() const;

  void SetContent(StatusConverter::ContentData content);
  StatusConverter::ContentData GetContent();

 public:
  static StatusDevice *New();
  vtkTypeMacro(StatusDevice,Device);

  void PrintSelf(ostream& os, vtkIndent indent);

 protected:
  StatusDevice();
  ~StatusDevice();

 protected:
  igtl::StatusMessage::Pointer OutMessage;
  igtl::GetStatusMessage::Pointer GetMessage;

  StatusConverter::ContentData Content;
};


//---------------------------------------------------------------------------
class OPENIGTLINKIO_DEVICES_EXPORT StatusDeviceCreator : public DeviceCreator
{
public:
  virtual DevicePointer Create(std::string device_name);
  virtual std::string GetDeviceType() const;

  static StatusDeviceCreator *New();
  vtkTypeMacro(StatusDeviceCreator,vtkObject);
};

} // namespace igtlio


#endif // IGTLIOSTATUSDEVICE_H
