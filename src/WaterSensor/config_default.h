#define WifiSSID "WifiSSID"
#define WifiPassword "WifiPassword"
#define Server "Server"
#define endpoint "endpoint"
#define Port 0
#define auth "auth"
#define OtaUrl      "OtaUrl"
#define OtaUser     "OtaUser"
#define OtaPassword "OtaPassword"

// Kořenový certifikát (CA), proti kterému se ověřují TLS certifikáty serverů
// (OTA i odesílání snímků).
#define RootCA R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF"