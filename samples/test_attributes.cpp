/// Data structure with formatted fields
class FormattedData {
public:
  /// Unique identifier for this data
  int id [[format("%d")]];

  /// Human-readable name
  const char *name [[format("%s")]];

  /// Floating point value
  float value [[format("%.2f")]];

  /// Boolean flag indicating if this is enabled
  bool enabled [[format("%d")]];

  /// Pointer to arbitrary data
  void *ptr [[format("%p")]];

  /// Unix timestamp in milliseconds
  unsigned long long timestamp [[format("%llu")]];
};

/// Network packet for communication
class NetworkPacket {
public:
  /// Unique packet identifier (hex format)
  unsigned int packet_id [[format("0x%08X")]];

  /// Network port number
  unsigned short port [[format("%u")]];

  /// IP address or hostname
  const char *address [[format("%s")]];

  /// Packet header information
  struct Header {
    /// Protocol version number
    unsigned char version [[format("%d")]];

    /// Packet flags (bitmask)
    unsigned char flags [[format("0x%02X")]];
  } header;
};

/// Type of network packet
enum PacketType {
  DATA = 0,    ///< Data packet
  CONTROL = 1, ///< Control packet
  ERROR = 2    ///< Error packet
};