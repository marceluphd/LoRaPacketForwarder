#ifndef LINUX
        #define LINUX
#endif

#include <cstdio>
#include <ctime>
#include <csignal>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include "smtUdpPacketForwarder/ConfigFileParser.h"
#include "smtUdpPacketForwarder/UdpUtils.h"

#include <LoRaLib.h>


static volatile sig_atomic_t keepRunning = 1;

static LoRaPacketTrafficStats_t loraPacketStats;
static SX127x *lora;

void uplinkPacketSenderWorker() { // {{{

  static std::function<bool(char*, int, char*, int)> isValidAck =
          [](char* origMsg, int origMsgSz, char* respMsg, int respMsgSz) {
              if (origMsg != nullptr && origMsgSz > 4 && respMsg != nullptr && respMsgSz >= 4) {
                return origMsg[0] == respMsg[0] && origMsg[1] == respMsg[1] &&
                         origMsg[2] == respMsg[2] && respMsg[3] == PKT_PUSH_ACK;
              }
              return false;
  };

  bool iterateOnceMore = false;
  do
  {
    PackagedDataToSend_t packet{DequeuePacket()};
    iterateOnceMore = (packet.data_len > 0);
    if (iterateOnceMore)
    {
      bool result = SendUdp(packet.destination,
          reinterpret_cast<char*>(packet.data.get()), packet.data_len, isValidAck);

      if (!result)
      {
        time_t currTime{std::time(nullptr)};
        char asciiTime[25];
        std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));
        printf("(%s) No uplink ACK received from %s\n", asciiTime, packet.destination.address.c_str());
        if (RequeuePacket(std::move(packet), 4))
        { printf("(%s) Requeued the uplink packet.\n",  asciiTime); }
        fflush(stdout);
      }
    }

    if (keepRunning) std::this_thread::sleep_for(std::chrono::milliseconds(150));
  } while (keepRunning || iterateOnceMore);

} // }}}

void hexPrint(uint8_t data[], int length) { // {{{

  if (length < 1) {
    printf("\n");
    fflush(stdout);
    return;
  }

  for (int i = 0; i < length; i += 16) {
    printf("  %04x  ", (i * 16));
    int j = i;
    for (int limit = i + 16 ; j < limit && j < length; ++j) {
      if (j % 8 == 0) printf("  ");
      printf("%02x ", data[j]);
    }

    for (int k = j; k % 16 != 0; ++k) {
      if (k % 8 == 0) printf("  ");
      printf("   ");
    }

    printf("  ");

    for (int k = i; k < j; ++k)
      printf("%c", isprint(data[k]) ? data[k] : '.');

    printf("\n");
  }
  printf("\n");
  fflush(stdout);
} // }}}

enum class LoRaRecvStat { NODATA, DATARECV, DATARECVFAIL };

LoRaRecvStat receiveData(bool receiveOnAllChannels, LoRaDataPkt_t &pkt, uint8_t msg[]) { // {{{

  int state = ERR_RX_TIMEOUT;
  bool insistDataReceiveFailure = false;

  if (!receiveOnAllChannels) {
    state = lora->receive(msg, SX127X_MAX_PACKET_LENGTH);
  } else {
    for (unsigned i = SpreadingFactor_t::SF7; i <= SpreadingFactor_t::SF_MAX; ++i) {
      lora->setSpreadingFactor(i);
      state = lora->scanChannel();
      if (state == PREAMBLE_DETECTED) /*&& lora->getRSSI() > -124.0) */{
        state = lora->receive(msg, SX127X_MAX_PACKET_LENGTH);
        insistDataReceiveFailure = (state != ERR_NONE);
        printf("Got preamble at SF%d, RSSI %f!\n", i, lora->getRSSI());
        break;
      }
    }
  }

  time_t timestamp{std::time(nullptr)};
  char asciiTime[25];

  std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&timestamp));

  if (state == ERR_NONE) { // packet was successfully received
    int msg_length = lora->getPacketLength(false);

    ++loraPacketStats.recv_packets;
    ++loraPacketStats.recv_packets_crc_good;


    printf("\n(%s) Received packet:\n", asciiTime);

    // Received Signal Strength Indicator of the last received packet
    printf(" RSSI:\t\t\t%.1f dBm\n", lora->getRSSI());

    // SNR (Signal-to-Noise Ratio) of the last received packet
    printf(" SNR:\t\t\t%f dB\n", lora->getSNR());

    // frequency error of the last received packet
    printf(" Frequency error:\t%f Hz\n", lora->getFrequencyError());

    printf(" Data:\t\t\t%d bytes\n\n", msg_length);
    hexPrint(msg, msg_length);


    pkt.RSSI = lora->getRSSI();
    pkt.SNR = lora->getSNR();
    pkt.msg = static_cast<const uint8_t*> (msg);
    pkt.msg_sz = msg_length;

    return LoRaRecvStat::DATARECV;

  } else if (state == ERR_CRC_MISMATCH) {
    // packet was received, but is malformed
    ++loraPacketStats.recv_packets;
    printf("(%s) Received packet CRC error - ignored!\n", asciiTime);
    fflush(stdout);
    return LoRaRecvStat::DATARECVFAIL;
  }

  return (insistDataReceiveFailure ? LoRaRecvStat::DATARECVFAIL : LoRaRecvStat::NODATA);
} // }}}

uint16_t restartLoRaChip(PlatformInfo_t &cfg) { // {{{

  if (cfg.lora_chip_settings.pin_rest > -1) {
    lora->reset();
    delay(10); // wait for the automatic callibration to finish
  }

  int8_t power = 17, currentLimit_ma = 100, gain = 0;

  return lora->begin(
    cfg.lora_chip_settings.carrier_frequency_mhz,
    cfg.lora_chip_settings.bandwidth_khz,
    cfg.lora_chip_settings.spreading_factor,
    cfg.lora_chip_settings.coding_rate,
    cfg.lora_chip_settings.sync_word,
    power,
    currentLimit_ma,
    cfg.lora_chip_settings.preamble_length,
    gain
  );
} // }}}

SX127x* instantiateLoRa(LoRaChipSettings_t& lora_chip_settings) // {{{
{
  static const std::map<std::string, std::function<SX127x*(LoRa*)> > LORA_CHIPS {
    { "SX1272", [](LoRa* lora_module_settings) { return new SX1272(lora_module_settings); } },
    { "SX1273", [](LoRa* lora_module_settings) { return new SX1273(lora_module_settings); } },
    { "SX1276", [](LoRa* lora_module_settings) { return new SX1276(lora_module_settings); } },
    { "SX1277", [](LoRa* lora_module_settings) { return new SX1277(lora_module_settings); } },
    { "SX1278", [](LoRa* lora_module_settings) { return new SX1278(lora_module_settings); } },
    { "SX1279", [](LoRa* lora_module_settings) { return new SX1279(lora_module_settings); } },
    { "RFM95", [](LoRa* lora_module_settings) { return new RFM95(lora_module_settings); } },
    { "RFM96", [](LoRa* lora_module_settings) { return new RFM96(lora_module_settings); } },
    { "RFM97", [](LoRa* lora_module_settings) { return new RFM97(lora_module_settings); } },
    { "RFM98", [](LoRa* lora_module_settings) { return new RFM96(lora_module_settings); } } // LIKE RFM96
  };

  LoRa* module_settings = new LoRa(
    lora_chip_settings.pin_nss_cs,
    lora_chip_settings.pin_dio0,
    lora_chip_settings.pin_dio1,
    (lora_chip_settings.pin_rest > -1 ? lora_chip_settings.pin_rest : RADIOLIB_NC)
  );

  return LORA_CHIPS.at(lora_chip_settings.ic_model)(module_settings);
} // }}}

int main(int argc, char **argv) {

  time_t currTime{std::time(nullptr)};
  char asciiTime[25];

  std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));
  printf("(%s) Started %s...\n", asciiTime, argv[0]);

  char networkIfaceName[64] = "eth0";
  char gatewayId[25];
  memset(gatewayId, 0, sizeof(gatewayId));

  if (argc > 1) {
    strcpy(networkIfaceName, (const char*) argv[1]);
  }

  PlatformInfo_t cfg = LoadConfiguration("./config.json");

  for (auto &serv : cfg.servers) {
    serv.network_cfg =
       PrepareNetworking(networkIfaceName, serv.receive_timeout_ms * 1000, gatewayId);
  }

  SetGatewayIdentifier(cfg, gatewayId);
  PrintConfiguration(cfg);

  SPISettings spiSettings{cfg.lora_chip_settings.spi_speed_hz, MSBFIRST,
    SPI_MODE0, cfg.lora_chip_settings.spi_channel};
  SPI.beginTransaction(spiSettings);

  lora = instantiateLoRa(cfg.lora_chip_settings);

  uint16_t state = ERR_NONE + 1;

  for (uint8_t c = 0; state != ERR_NONE && c < 200; ++c) {
    state = restartLoRaChip(cfg);

    if (state == ERR_NONE) printf("LoRa chip setup succeeded!\n\n");
    else printf("LoRa chip setup failed, code %d\n", state);
  }

  if (state != ERR_NONE) {
    currTime = std::time(nullptr);
    std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));
    printf("Giving up due to failing LoRa chip setup!\n(%.24s) Exiting!\n", asciiTime);
    SPI.endTransaction();
    return 1;
  }

  fflush(stdout);


  auto signalHandler = [](int sigNum) { keepRunning = 0; };

  signal(SIGHUP, signalHandler);  // Process' terminal is closed, the
                                  // user has logger out, etc.
  signal(SIGINT, signalHandler);  // Interrupted process (Ctrl + c)
  signal(SIGQUIT, signalHandler); // Quit request (via console Ctrl + \)
  signal(SIGTERM, signalHandler); // Termination request
  signal(SIGXFSZ, signalHandler); // Creation of a file so large that
                                  // it's not allowed anymore to grow

  bool receiveOnAllChannels = cfg.lora_chip_settings.all_spreading_factors;

  const uint16_t delayIntervalMs = 20;
  const uint32_t sendStatPktIntervalSeconds = 420;
  const uint32_t loraChipRestIntervalSeconds = 2700;

  time_t nextStatUpdateTime = std::time(nullptr) - 1;
  time_t nextChipRestTime = nextStatUpdateTime + 1 + loraChipRestIntervalSeconds;

  LoRaDataPkt_t loraDataPacket;
  uint8_t msg[SX127X_MAX_PACKET_LENGTH];

  std::thread uplinkSender{uplinkPacketSenderWorker};

  while (keepRunning) {
    currTime = std::time(nullptr);

    if (keepRunning && currTime >= nextStatUpdateTime) {
      nextStatUpdateTime = currTime + sendStatPktIntervalSeconds;
      std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));
      printf("(%s) Sending stat update to server(s)... ", asciiTime);
      PublishStatProtocolPacket(cfg, loraPacketStats);
      ++loraPacketStats.forw_packets_crc_good;
      ++loraPacketStats.forw_packets;
      printf("done\n");
      fflush(stdout);
    }

    if (!keepRunning) break;

    LoRaRecvStat lastRecvResult = receiveData(receiveOnAllChannels, loraDataPacket, msg);

    if (lastRecvResult == LoRaRecvStat::DATARECV) {
      PublishLoRaProtocolPacket(cfg, loraDataPacket);
    } else if (keepRunning && lastRecvResult == LoRaRecvStat::NODATA) {

      if (cfg.lora_chip_settings.pin_rest > -1 && currTime >= nextChipRestTime) {
        nextChipRestTime = currTime + loraChipRestIntervalSeconds;

        do {
          state = restartLoRaChip(cfg);
          std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));
          printf("(%s) Regular LoRa chip reset done - code %d, %s success\n",
               asciiTime, state, (state == ERR_NONE ? "with" : "WITHOUT"));
	  fflush(stdout);
          delay(delayIntervalMs);
        } while (state != ERR_NONE);

      } else if (!receiveOnAllChannels) {
        delay(delayIntervalMs);
      }

    }

  }

  currTime = std::time(nullptr);
  std::strftime(asciiTime, sizeof(asciiTime), "%c", std::localtime(&currTime));

  printf("\n(%s) Shutting down...\n", asciiTime);
  fflush(stdout);
  SPI.endTransaction();
  uplinkSender.join();
}
