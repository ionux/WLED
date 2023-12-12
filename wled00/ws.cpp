#include "wled.h"

/*
 * WebSockets server for bidirectional communication
 */
#ifdef WLED_ENABLE_WEBSOCKETS

uint16_t wsLiveClientId = 0;
unsigned long wsLastLiveTime = 0;

//uint8_t* wsFrameBuffer = nullptr;

#define WS_LIVE_INTERVAL 40

void wsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
  if (type == WS_EVT_CONNECT)
  {
    //client connected
#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("WS client connected."));
#endif

    sendDataWs(client);
  }
  else if (type == WS_EVT_DISCONNECT)
  {
    //client disconnected
    if (client->id() == wsLiveClientId)
    {
      wsLiveClientId = 0;
    }

#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("WS client disconnected."));
#endif
  }
  else if (type == WS_EVT_DATA)
  {
    // data packet
    AwsFrameInfo *info = (AwsFrameInfo*)arg;

    if (info->final && (info->index == 0) && (info->len == len))
    {
      // the whole message is in a single frame and we got all of its data (max. 1450 bytes)
      if (info->opcode == WS_TEXT)
      {
        if ((len > 0) && (len < 10) && (data[0] == 'p'))
        {
          // application layer ping/pong heartbeat.
          // client-side socket layer ping packets are unresponded (investigate)
          client->text(F("pong"));

          return;
        }

        bool verboseResponse = false;

        if (!requestJSONBufferLock(11))
        {
          return;
        }

        DeserializationError error = deserializeJson(doc, data, len);

        JsonObject root = doc.as<JsonObject>();

        if (error || root.isNull())
        {
          releaseJSONBufferLock();

          return;
        }

        if (root["v"] && root.size() == 1)
        {
          //if the received value is just "{"v":true}", send only to this client
          verboseResponse = true;
        }
        else if (root.containsKey("lv"))
        {
          wsLiveClientId = root["lv"] ? client->id() : 0;
        }
        else
        {
          verboseResponse = deserializeState(root);
        }

        releaseJSONBufferLock(); // will clean fileDoc

        if (!interfaceUpdateCallMode)
        {
          // individual client response only needed if no WS broadcast soon
          if (verboseResponse)
          {
            sendDataWs(client);
          }
          else
          {
            // we have to send something back otherwise WS connection closes
            client->text(F("{\"success\":true}"));
          }
        }
      }
    }
    else
    {
      if ((info->index + len) == info->len)
      {
        if (info->final)
        {
          if (info->message_opcode == WS_TEXT)
          {
            client->text(F("{\"error\":9}")); // ERR_JSON we do not handle split packets right now
          }
        }
      }

#ifdef WLED_DEBUG
      DEBUG_PRINTLN(F("WS multipart message."));
#endif
    }
  }
  else if (type == WS_EVT_ERROR)
  {
    //error was received from the other end
#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("WS error."));
#endif
  }
  else if (type == WS_EVT_PONG)
  {
    //pong message was received (in response to a ping request maybe)
#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("WS pong."));
#endif
  }
  else
  {
    // RLM - addressing code smell
  }
}


void sendDataWs(AsyncWebSocketClient *client)
{
  if (!ws.count())
  {
    return;
  }

  if (!requestJSONBufferLock(12))
  {
    return;
  }

  AsyncWebSocketMessageBuffer *buffer;

  JsonObject state = doc.createNestedObject("state");
  serializeState(state);

  JsonObject info  = doc.createNestedObject("info");
  serializeInfo(info);

  size_t len = measureJson(doc);

#ifdef WLED_DEBUG
  DEBUG_PRINTF("JSON buffer size: %u for WS request (%u).\n", doc.memoryUsage(), len);
#endif

  size_t heap1 = ESP.getFreeHeap();

#ifdef WLED_DEBUG
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
#endif

#ifdef ESP8266
  if (len > heap1)
  {
#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("Out of memory (WS)!"));
#endif

    return;
  }
#endif

  buffer = ws.makeBuffer(len); // will not allocate correct memory sometimes on ESP8266
                               // RLM - investigate

#ifdef ESP8266
  size_t heap2 = ESP.getFreeHeap();

#ifdef WLED_DEBUG
  DEBUG_PRINT(F("heap ")); DEBUG_PRINTLN(ESP.getFreeHeap());
#endif
#else
  size_t heap2 = 0; // ESP32 variants do not have the same issue and will work without checking heap allocation
#endif

  if (!buffer || ((heap1 - heap2) < len))
  {
    releaseJSONBufferLock();

#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("WS buffer allocation failed."));
#endif

    ws.closeAll(1013); //code 1013 = temporary overload, try again later
    ws.cleanupClients(0); //disconnect all clients to release memory
    ws._cleanBuffers();

    return; //out of memory
  }

  buffer->lock();

  // RLM - check return code
  serializeJson(doc, (char *)buffer->get(), len);

#ifdef WLED_DEBUG
  DEBUG_PRINT(F("Sending WS data "));
#endif

  if (client)
  {
    client->text(buffer);

#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("to a single client."));
#endif
  }
  else
  {
    ws.textAll(buffer);

#ifdef WLED_DEBUG
    DEBUG_PRINTLN(F("to multiple clients."));
#endif
  }

  buffer->unlock();

  ws._cleanBuffers();

  releaseJSONBufferLock();
}

bool sendLiveLedsWs(uint32_t wsClient)
{
  AsyncWebSocketClient *wsc = ws.client(wsClient);

  if (!wsc || (wsc->queueLength() > 0))
  {
    return false; //only send if queue free
  }

  size_t used = strip.getLengthTotal();

#ifdef ESP8266
  const size_t MAX_LIVE_LEDS_WS = 256U;
#else
  const size_t MAX_LIVE_LEDS_WS = 1024U;
#endif

  // RLM - refactor div
  size_t n = ((used - 1) / MAX_LIVE_LEDS_WS) + 1; //only serve every n'th LED if count over MAX_LIVE_LEDS_WS

  size_t pos = ((strip.isMatrix) ? 4 : 2);  // start of data

  // RLM - refactor div
  size_t bufSize = pos + ((used / n) * 3);

  AsyncWebSocketMessageBuffer * wsBuf = ws.makeBuffer(bufSize);

  if (!wsBuf)
  {
    return false; //out of memory
  }

  uint8_t* buffer = wsBuf->get();

  // RLM - check if buffer is null
  buffer[0] = 'L';
  buffer[1] = 1; //version

#ifndef WLED_DISABLE_2D
  size_t skipLines = 0;

  if (strip.isMatrix)
  {
    buffer[1] = 2; //version
    buffer[2] = Segment::maxWidth;
    buffer[3] = Segment::maxHeight;

    if (used > (MAX_LIVE_LEDS_WS * 4))
    {
      // RLM - refactor divs
      buffer[2] = Segment::maxWidth / 4;
      buffer[3] = Segment::maxHeight / 4;

      skipLines = 3;
    }
    else if (used > MAX_LIVE_LEDS_WS)
    {
      // RLM - refactor divs
      buffer[2] = Segment::maxWidth / 2;
      buffer[3] = Segment::maxHeight / 2;

      skipLines = 1;
    }
    else
    {
      // RLM - addressing code smell
    }
  }
#endif

  for (size_t i = 0; pos < (bufSize - 2); i += n)
  {
#ifndef WLED_DISABLE_2D
    if (strip.isMatrix && skipLines)
    {
      // RLM - refactor divs and mod
      if ((i / Segment::maxWidth) % (skipLines + 1))
      {
        i += Segment::maxWidth * skipLines;
      }
    }
#endif

    uint32_t c = strip.getPixelColor(i);

    uint8_t r = R(c);
    uint8_t g = G(c);
    uint8_t b = B(c);
    uint8_t w = W(c);

    // RLM - possible refactor candidate
    buffer[pos++] = scale8(qadd8(w, r), strip.getBrightness()); //R, add white channel to RGB channels as a simple RGBW -> RGB map
    buffer[pos++] = scale8(qadd8(w, g), strip.getBrightness()); //G
    buffer[pos++] = scale8(qadd8(w, b), strip.getBrightness()); //B
  }

  wsc->binary(wsBuf);

  return true;
}


void handleWs()
{
  if ((millis() - wsLastLiveTime) > WS_LIVE_INTERVAL)
  {
#ifdef ESP8266
    ws.cleanupClients(3);
#else
    ws.cleanupClients();
#endif

    bool success = true;

    if (wsLiveClientId)
    {
      success = sendLiveLedsWs(wsLiveClientId);
    }

    wsLastLiveTime = millis();

    if (!success)
    {
      wsLastLiveTime -= 20; //try again in 20ms if failed due to non-empty WS queue
    }
  }
}

#else
void handleWs() {}
void sendDataWs(AsyncWebSocketClient * client) {}
#endif
