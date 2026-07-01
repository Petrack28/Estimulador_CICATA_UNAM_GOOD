#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>

// --- PINES ---
const int BUZZER_PIN = 11;
const int CRITICAL_PIN = 35;
const int MOSFET_PIN = 18;
const int I2C_SDA_PIN = 33;
const int I2C_SCL_PIN = 34;

const int POT_I2C_ADDRESS = 0x2C;
volatile int valorPotenciometro = 150;
volatile int anchoDePulsoUs = 250;      // tiempo en ALTO
volatile int tiempoBajoUs = 19750;      // tiempo en BAJO (por defecto = 50Hz)
volatile unsigned long duracionPruebaMs = 30 * 60000UL;

volatile bool estimulacionActiva = false;
volatile bool stepUpEncendido = false;
unsigned long tiempoInicioEstimulacion = 0;
TaskHandle_t tareaHandle = NULL;

Preferences prefs;

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
const unsigned long TIMEOUT_HEARTBEAT_MS = 10000; // 10 segundos de margen para red en la nube

// ---------------- NVS: GUARDAR Y CARGAR VALORES ----------------
void cargarValoresNVS() {
  prefs.begin("estimulador", false);
  valorPotenciometro = prefs.getInt("pot", 150);
  anchoDePulsoUs     = prefs.getInt("pulsoAlto", 250);
  tiempoBajoUs       = prefs.getInt("pulsoBajo", 19750);
  duracionPruebaMs   = (unsigned long)prefs.getInt("duracion", 30) * 60000UL;
  prefs.end();
  Serial.printf("Valores cargados: pot=%d, alto=%d, bajo=%d, dur=%lums\n",
                valorPotenciometro, anchoDePulsoUs, tiempoBajoUs, duracionPruebaMs);
}

void guardarValoresNVS() {
  prefs.begin("estimulador", false);
  prefs.putInt("pot",       valorPotenciometro);
  prefs.putInt("pulsoAlto", anchoDePulsoUs);
  prefs.putInt("pulsoBajo", tiempoBajoUs);
  prefs.putInt("duracion",  (int)(duracionPruebaMs / 60000UL));
  prefs.end();
  Serial.println("Valores guardados en NVS.");
}

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

void beepEncendido() {
  // BIP simple al encender
  tone(BUZZER_PIN, 880);
  delay(200);
  noTone(BUZZER_PIN);
}

void doubleBipStepUp() {
  // Doble BIP indicando que el Step-Up se va a encender
  tone(BUZZER_PIN, 880); delay(150); noTone(BUZZER_PIN); delay(100);
  tone(BUZZER_PIN, 880); delay(150); noTone(BUZZER_PIN);
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
      delayMicroseconds(tiempoBajoUs);  // tiempo en BAJO independiente
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
    guardarValoresNVS();
  } else if (cmd == "PULSO_ALTO") {
    int v = constrain(val.toInt(), 50, 19000);
    anchoDePulsoUs = v;
    guardarValoresNVS();
  } else if (cmd == "PULSO_BAJO") {
    int v = constrain(val.toInt(), 50, 100000);
    tiempoBajoUs = v;
    guardarValoresNVS();
  } else if (cmd == "DURACION") {
    duracionPruebaMs = (unsigned long)val.toInt() * 60000UL;
    guardarValoresNVS();
  } else if (cmd == "ESTIM") {
    if (val == "1") iniciarEstimulacion(); else detenerEstimulacion();
  }
}

void enviarStatus() {
  String wifiConectado = (WiFi.status() == WL_CONNECTED) ? "1" : "0";
  String ssidActual = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : "N/A";
  String mqttConectado = mqttClient.connected() ? "1" : "0";
  float frecuencia = 1000000.0 / (anchoDePulsoUs + tiempoBajoUs);

  // Tiempo transcurrido en segundos (0 si no está estimulando)
  unsigned long transcurrido = 0;
  if (estimulacionActiva) {
    transcurrido = (millis() - tiempoInicioEstimulacion) / 1000;
  }

  String s = String(stepUpEncendido ? "1" : "0") + "," +
             String(estimulacionActiva ? "1" : "0") + "," +
             String(valorPotenciometro) + "," +
             String(anchoDePulsoUs) + "," +
             String(tiempoBajoUs) + "," +
             String(duracionPruebaMs / 60000UL) + "," +
             wifiConectado + "," +
             ssidActual + "," +
             mqttConectado + "," +
             String(frecuencia, 2) + "," +
             String(transcurrido); // segundos transcurridos
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
  digitalWrite(CRITICAL_PIN, LOW); // Step-Up apagado al inicio por seguridad
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, LOW);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Cargar valores guardados en memoria flash
  cargarValoresNVS();

  // PASO 1: BIP de encendido
  beepEncendido();
  Serial.println("Dispositivo encendido.");

  // PASO 2: Conectar WiFi y MQTT (respetando lógica actual)
  if (!conectarWiFi()) {
    Serial.println("No se logró conectar al WiFi en el arranque. Reintentando en loop()...");
  } else {
    conectarMQTT();

    // Star Wars: WiFi + MQTT conectados exitosamente
    Serial.println("WiFi y MQTT conectados. Sonando Star Wars...");
    melodiaStarWars();

    // PASO 3: esperar 5s y doble BIP → encender Step-Up
    Serial.println("Esperando 5 segundos antes de encender Step-Up...");
    delay(5000);
    doubleBipStepUp();
    encenderStepUp();
    Serial.println("Step-Up encendido.");

    // PASO 4: Esperar 10 segundos para estabilización del Step-Up
    Serial.println("Esperando 10 segundos para estabilización...");
    programarPotenciometro(valorPotenciometro);
    delay(10000);

    // Mario: dispositivo listo para usar
    Serial.println("¡Listo! Esperando comando de estimulación desde la app.");
    melodiaMQTTConectado();
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
