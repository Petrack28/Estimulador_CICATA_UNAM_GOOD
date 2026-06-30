#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// --- PINES ---
const int BUZZER_PIN = 11;
const int CRITICAL_PIN = 35;
const int MOSFET_PIN = 18;
const int I2C_SDA_PIN = 33;
const int I2C_SCL_PIN = 34;

const int POT_I2C_ADDRESS = 0x2C;
volatile int valorPotenciometro = 150;
volatile int anchoDePulsoUs = 250;
volatile unsigned long duracionPruebaMs = 30 * 60000UL;

volatile bool estimulacionActiva = false;
volatile bool stepUpEncendido = false;
unsigned long tiempoInicioEstimulacion = 0;
TaskHandle_t tareaHandle = NULL;

// --- WIFI ---
// Ya no se define SSID/PASSWORD fijos: se configuran vía portal WiFiManager
WiFiManager wifiManager;

// --- HIVEMQ CLOUD ---
const char* MQTT_HOST = "92531f7c6f4c44b7b5442f0fc08bc15e.s1.eu.hivemq.cloud"; // tu cluster URL
const int MQTT_PORT = 8883;
const char* MQTT_USER = "Thor123";
const char* MQTT_PASS = "Estimulador123";

const char* TOPIC_CMD = "estimulador/cmd";
const char* TOPIC_STATUS = "estimulador/status";
const char* TOPIC_HEARTBEAT = "estimulador/heartbeat";

WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

// --- HEARTBEAT DE SEGURIDAD ---
unsigned long ultimoHeartbeat = 0;
const unsigned long TIMEOUT_HEARTBEAT_MS = 5000; // si no hay heartbeat en 5s, se detiene

// ---------------- SONIDOS ----------------
void melodiaBienvenida() {
  tone(BUZZER_PIN, 660); delay(250); noTone(BUZZER_PIN); delay(120);
  tone(BUZZER_PIN, 545); delay(250); noTone(BUZZER_PIN); delay(90);
  delay(1000);
}
void melodiaActivacion() {
  tone(BUZZER_PIN, 444); delay(130); noTone(BUZZER_PIN); delay(50);
  tone(BUZZER_PIN, 444); delay(130); noTone(BUZZER_PIN);
}
void melodiaFin() {
  for (int i = 0; i < 4; i++) { tone(BUZZER_PIN, 120); delay(130); noTone(BUZZER_PIN); delay(50); }
}

void beepCuentaRegresiva() {
  tone(BUZZER_PIN, 880); // beep agudo simple
  delay(200);
  noTone(BUZZER_PIN);
}

// --- Notas para la melodía de Star Wars ---
const int nC = 261, nD = 294, nE = 329, nF = 349, nG = 391, nGS = 415, nA = 440, nAS = 455, nB = 466;
const int nCH = 523, nCSH = 554, nDH = 587, nDSH = 622, nEH = 659, nFH = 698, nFSH = 740, nGH = 783, nGSH = 830, nAH = 880;

void melodiaStarWars() {
  tone(BUZZER_PIN, nA, 500); delay(550);
  tone(BUZZER_PIN, nA, 500); delay(550);
  tone(BUZZER_PIN, nA, 500); delay(550);
  tone(BUZZER_PIN, nF, 350); delay(400);
  tone(BUZZER_PIN, nCH, 150); delay(200);

  tone(BUZZER_PIN, nA, 500); delay(550);
  tone(BUZZER_PIN, nF, 350); delay(400);
  tone(BUZZER_PIN, nCH, 150); delay(200);
  tone(BUZZER_PIN, nA, 1000); delay(1050);

  tone(BUZZER_PIN, nEH, 500); delay(550);
  tone(BUZZER_PIN, nEH, 500); delay(550);
  tone(BUZZER_PIN, nEH, 500); delay(550);
  tone(BUZZER_PIN, nFH, 350); delay(400);
  tone(BUZZER_PIN, nCH, 150); delay(200);

  tone(BUZZER_PIN, nGS, 500); delay(550);
  tone(BUZZER_PIN, nF, 350); delay(400);
  tone(BUZZER_PIN, nCH, 150); delay(200);
  tone(BUZZER_PIN, nA, 1000); delay(1050);

  noTone(BUZZER_PIN);
}

// --- Melodía de Super Mario World (al conectar a MQTT) ---
int melodiaMario[] = {
  440, 440, 0, 440, 0, 349, 440, 0, 523, 0, 392, 0,
  349, 392, 0, 294, 392, 440, 415, 392, 349, 440, 523, 587,
  0, 349, 523, 440, 349, 392, 330
};
int duracionesMario[] = {
  8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4,
  8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4,
  4, 8, 8, 8, 8, 8, 4
};

void melodiaMQTTConectado() {
  int tempo = 900;
  int melodySize = sizeof(melodiaMario) / sizeof(melodiaMario[0]);
  for (int i = 0; i < melodySize; i++) {
    int noteDuration = tempo / duracionesMario[i];
    if (melodiaMario[i] == 0) {
      delay(noteDuration);
    } else {
      tone(BUZZER_PIN, melodiaMario[i], noteDuration);
      delay(noteDuration);
    }
    delay(noteDuration * 0.3);
  }
  noTone(BUZZER_PIN);
}

// ---------------- POTENCIOMETRO ----------------
void programarPotenciometro(int valor) {
  Wire.beginTransmission(POT_I2C_ADDRESS);
  Wire.write(0x00);
  Wire.write(valor);
  Wire.endTransmission();
}

// ---------------- TAREA THETA BURST ----------------
void tareaThetaBurst(void *pvParameters) {
  while (estimulacionActiva) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(MOSFET_PIN, HIGH);
      delayMicroseconds(anchoDePulsoUs);
      digitalWrite(MOSFET_PIN, LOW);
      delayMicroseconds(20000 - anchoDePulsoUs);
    }
    vTaskDelay(140 / portTICK_PERIOD_MS);
  }
  digitalWrite(MOSFET_PIN, LOW);
  tareaHandle = NULL;
  vTaskDelete(NULL);
}

void iniciarEstimulacion() {
  if (!stepUpEncendido || estimulacionActiva) return;
  estimulacionActiva = true;
  tiempoInicioEstimulacion = millis();
  ultimoHeartbeat = millis();
  melodiaActivacion();
  xTaskCreate(tareaThetaBurst, "Generador_TBS", 2048, NULL, 1, &tareaHandle);
}

void detenerEstimulacion() {
  estimulacionActiva = false;
  delay(200);
  digitalWrite(MOSFET_PIN, LOW);
  melodiaFin();
}

void encenderStepUp() {
  digitalWrite(CRITICAL_PIN, HIGH);
  stepUpEncendido = true;
}

void apagarStepUp() {
  detenerEstimulacion();
  digitalWrite(CRITICAL_PIN, LOW);
  stepUpEncendido = false;
}

// ---------------- MQTT CALLBACK ----------------
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == TOPIC_HEARTBEAT) {
    ultimoHeartbeat = millis();
    return;
  }

  int sep = msg.indexOf(':');
  if (sep == -1) return;
  String cmd = msg.substring(0, sep);
  String val = msg.substring(sep + 1);

  if (cmd == "POT") {
    int v = constrain(val.toInt(), 0, 255);
    valorPotenciometro = v;
    programarPotenciometro(v);
  } else if (cmd == "PULSO") {
    int v = constrain(val.toInt(), 50, 19000);
    anchoDePulsoUs = v;
  } else if (cmd == "DURACION") {
    duracionPruebaMs = (unsigned long)val.toInt() * 60000UL;
  } else if (cmd == "STEPUP") {
    if (val == "1") encenderStepUp(); else apagarStepUp();
  } else if (cmd == "ESTIM") {
    if (val == "1") iniciarEstimulacion(); else detenerEstimulacion();
  }
}

void enviarStatus() {
  String wifiConectado = (WiFi.status() == WL_CONNECTED) ? "1" : "0";
  String ssidActual = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "N/A";
  String mqttConectado = mqttClient.connected() ? "1" : "0";

  String s = String(stepUpEncendido ? "1" : "0") + "," +
             String(estimulacionActiva ? "1" : "0") + "," +
             String(valorPotenciometro) + "," +
             String(anchoDePulsoUs) + "," +
             String(duracionPruebaMs / 60000UL) + "," +
             wifiConectado + "," +
             ssidActual + "," +
             mqttConectado;
  mqttClient.publish(TOPIC_STATUS, s.c_str());
}

const int MAX_INTENTOS_WIFI = 5;

bool conectarWiFi() {
  // Si ya hay credenciales guardadas, intenta conectar directo (rápido)
  wifiManager.setConnectTimeout(10); // segundos por intento
  wifiManager.setConfigPortalTimeout(180); // 3 min con el portal abierto antes de reintentar solo

  for (int intento = 1; intento <= MAX_INTENTOS_WIFI; intento++) {
    Serial.printf("Intento de conexión WiFi #%d de %d\n", intento, MAX_INTENTOS_WIFI);

    // autoConnect: intenta con credenciales guardadas; si no hay o fallan,
    // abre el portal "Estimulador_Setup" para que configures desde el teléfono
    bool exito = wifiManager.autoConnect("Estimulador_Setup");

    if (exito && WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
      Serial.println("Red: " + WiFi.SSID());
      melodiaStarWars();
      return true;
    }

    Serial.println("\nFallo el intento. Reintentando...");
    delay(500);
  }

  Serial.println("No se pudo conectar al WiFi tras 5 intentos.");
  return false;
}

void conectarMQTT() {
  secureClient.setInsecure(); // Para producción real, usa el certificado CA de HiveMQ
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setBufferSize(512);   // buffer más grande para TLS estable
  mqttClient.setKeepAlive(60);     // más tolerante a pequeños cortes

  while (!mqttClient.connected()) {
    Serial.print("Heap libre: ");
    Serial.println(ESP.getFreeHeap());
    Serial.println("Conectando a HiveMQ Cloud...");
    if (mqttClient.connect("ESP32_Estimulador", MQTT_USER, MQTT_PASS,
                            TOPIC_STATUS, 1, true, "OFFLINE")) {
      Serial.println("Conectado a MQTT");
      mqttClient.subscribe(TOPIC_CMD);
      mqttClient.subscribe(TOPIC_HEARTBEAT);
      melodiaMQTTConectado(); // confirma conexión MQTT con tema de Mario
    } else {
      Serial.print("Fallo, rc=");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("Iniciando...");

  pinMode(CRITICAL_PIN, OUTPUT);
  digitalWrite(CRITICAL_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  melodiaBienvenida();

  Serial.println("\n--- ESPERA DE SEGURIDAD: 20 SEGUNDOS ---");
  for (int i = 20; i > 0; i--) {
    Serial.printf("Iniciando en... %d\n", i);
    delay(1000);
  }
  beepCuentaRegresiva(); // pitido al terminar la cuenta regresiva

  programarPotenciometro(valorPotenciometro);

  if (!conectarWiFi()) {
    Serial.println("No se logró conectar al WiFi en el arranque. Reintentando en loop()...");
  } else {
    conectarMQTT();
  }
}

// ---------------- LOOP ----------------
unsigned long ultimoStatus = 0;

void loop() {
  // Si se cae el WiFi, reconectar antes de intentar nada de MQTT
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Reconectando...");
    // Por seguridad: si estaba estimulando, detener mientras no hay supervisión por red
    if (estimulacionActiva) {
      detenerEstimulacion();
      Serial.println("Estimulación detenida por pérdida de WiFi.");
    }
    if (conectarWiFi()) {
      conectarMQTT();
    } else {
      // Esperamos un poco antes de volver a intentar el ciclo completo de 5 intentos
      delay(5000);
    }
    return; // evita seguir ejecutando el resto del loop sin red
  }

  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  // Seguridad: si se está estimulando y se pierde el heartbeat de la app, detener
  if (estimulacionActiva && (millis() - ultimoHeartbeat > TIMEOUT_HEARTBEAT_MS)) {
    Serial.println("¡Heartbeat perdido! Deteniendo estimulación por seguridad.");
    detenerEstimulacion();
  }

  if (estimulacionActiva) {
    if (millis() - tiempoInicioEstimulacion >= duracionPruebaMs) {
      detenerEstimulacion();
      Serial.println("\n¡Prueba finalizada por tiempo!");
    }
  }

  if (millis() - ultimoStatus > 1000) {
    enviarStatus();
    ultimoStatus = millis();
  }
}
