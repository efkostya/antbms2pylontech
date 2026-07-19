#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h> 
#include <Update.h> // ОТА

// --- НАСТРОЙКИ КОЛИЧЕСТВА ЯЧЕЕК ---
#define NUM_CELLS 16  

// --- НАСТРОЙКИ ПИНОВ UART ---
#define RXD1 27 
#define TXD1 26
#define RXD2 16
#define TXD2 17
#define RS485_DE_RE 4 

#define ANT_PACKET_SIZE 140
const uint8_t antRequest[6] = {0x5A, 0x5A, 0x00, 0x00, 0x00, 0x00};
const uint8_t antHeader[4]  = {0xAA, 0x55, 0xAA, 0xFF};

unsigned long lastBmsRequestTime = 0;
const unsigned long bmsInterval = 2000;

// --- ЖУРНАЛ ИНВЕРТОРА ---
String inverter_log[6] = {"", "", "", "", "", ""};

uint8_t antBuffer[ANT_PACKET_SIZE];
String pylonInputBuffer = "";

// --- НАСТРОЙКИ WI-FI И MQTT ---
const char* WIFI_SSID = "TSPNET";
const char* WIFI_PASSWORD = "059hfBML";
const unsigned long WIFI_TIMEOUT_MS = 15000; 

const int MQTT_PORT = 1883;
const char* MQTT_USER = "efko@bk.ru";               
const char* MQTT_PASS = "kolbaska";               
const char* MQTT_CLIENT_ID = "ESP32_BMS_Gateway";

unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublishTime = 0;
const unsigned long mqttPublishInterval = 5000; 

WebServer server(80);
Preferences preferences;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- КОНСТАНТЫ СТАТУСА PYLONTECH ---
#define STATUS_NORMAL        0xC0
#define STATUS_FORCE_CHARGE  0xE0

// --- СТРОКОВЫЕ МАССИВЫ ---
const char* MOSFET_Charge_St[] = {
  "OFF", "ON", "overcharge", "overcurrent", "batt full", "pack overvoltage",
  "bat overtemp", "MOSFET overtemp", "abnormal current", "bat not detected",
  "PCB overtemp", "11-undefined", "12-undefined", "Discharge MOSFET abnormality", "14", "Manual off"
};

const char* MOSFET_Discharge_St[] = {
  "OFF", "ON", "cell overdischarge", "overcurrent", "4", "pack overdischarge",
  "bat overtemp", "MOSFET overtemp", "Abnormal current", "battery is not detected",
  "PCB overtemp", "charge MOSFET turn on", "shortcircuit", "Discharge MOSFET abnormality",
  "Start exception", "Manual off"
};

const char* Bal_St[] = {
  "OFF", "limit trigger exceeds", "charging v diff too high", "overtemp", "ACTIVE",
  "5-udef", "6-udef", "7-udef", "8-udef", "9-udef", "PCB Overtemp"
};

// --- СТРУКТУРА ДАННЫХ ---
struct PylonConfig {
  int voltages[NUM_CELLS];
  int temperatures[5] = {250, 250, 250, 250, 250}; 
  int16_t current = 0;                             
  int total_voltage = 52000;                       
  
  int remaining_capacity = 40000;                  
  int total_capacity = 50000;                      
  int real_remaining_capacity = 40000; 
  int real_total_capacity = 50000;     
  uint32_t cycle_capacity = 0;         

  int cycles = 0;
  int soc = 100; 
  byte status = STATUS_NORMAL;

  int32_t power = 0;
  byte mosfet_charge_st = 0;
  byte mosfet_discharge_st = 0;
  byte bal_st = 0;
  uint16_t bal_mask = 0;
  uint16_t cell_avg = 0;
  uint16_t cell_min = 0;
  uint16_t cell_max = 0;

  // Настройки, сохраняемые в память
  int charge_voltage_limit = 56800;
  int discharge_voltage_limit = 47500;
  int charge_current_limit = 1000;      
  int discharge_current_limit = -1000;  
  int fc_soc_threshold = 10;   
  int fc_cell_threshold = 2800; 
  bool manual_force_charge = false;
  int max_soc_threshold = 100; // По умолчанию заряжаем до 100%

  // ---> НОВЫЕ НАСТРОЙКИ MQTT <---
  String mqtt_ip = "192.168.3.106";
  String mqtt_topic = "antesp32/BMS/";

  // ---> НОВЫЕ ПЕРЕМЕННЫЕ ДЛЯ СЧЕТЧИКОВ ЭНЕРГИИ <---
  double total_charge_kwh = 111.0;
  double total_discharge_kwh = 130.0;
};

PylonConfig config;

// --- НАСТРОЙКИ FREERTOS ---
TaskHandle_t InverterTaskHandle;
SemaphoreHandle_t logMutex;

// --- ВЕБ-ИНТЕРФЕЙС (HTML) ---
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BMS Gateway</title>
  <style>
    body { font-family: Arial, sans-serif; background: #f4f4f9; padding: 20px; color: #333; }
    h2 { color: #0056b3; border-bottom: 2px solid #ccc; padding-bottom: 5px; margin-top: 25px; }
    h3 { color: #555; margin-bottom: 5px; font-size: 16px; }
    .card { background: white; padding: 15px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); margin-bottom: 20px; }
    label { display: block; margin-top: 10px; font-weight: bold; font-size: 14px; }
    input[type="number"], input[type="text"] { width: 100%; padding: 8px; margin-top: 5px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
    input[type="submit"], .btn { background: #28a745; color: white; border: none; padding: 10px 20px; font-size: 16px; border-radius: 4px; cursor: pointer; margin-top: 15px; width: 100%; text-align: center; text-decoration: none; display: inline-block; box-sizing: border-box; }
    input[type="submit"]:hover, .btn:hover { opacity: 0.9; }
    .stat { display: flex; justify-content: space-between; padding: 5px 0; border-bottom: 1px solid #eee; align-items: center; }
    .stat span { font-weight: bold; color: #0056b3; }
    .alert { color: red; font-weight: bold; }
    .manual-alert { color: #d32f2f; font-weight: bold; text-align: center; margin-top: 10px; }
    .badge-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(75px, 1fr)); gap: 10px; margin-top: 10px; }
    .badge { background: #e9ecef; padding: 8px; text-align: center; border-radius: 6px; font-size: 12px; font-weight: normal; color: #555; }
    .badge span { display: block; color: #0056b3; font-size: 14px; font-weight: bold; margin-top: 4px; }
    .log-box { font-family: 'Courier New', Courier, monospace; font-size: 13px; line-height: 1.6; color: #444; background: #e9ecef; padding: 10px; border-radius: 6px; }
  </style>
</head>
<body>
  <h2>Статус АКБ (ANT BMS)</h2>
  <div class="card">
    <div class="stat">Общее напряжение: <span><span id="v_tot">--</span> В</span></div>
    <div class="stat">Текущий ток: <span><span id="i_cur">--</span> А</span></div>
    <div class="stat">Мощность: <span><span id="pow">--</span> Вт</span></div>
    <div class="stat">Уровень заряда (SoC): <span><span id="soc">--</span> %</span></div>
    <div class="stat">Полная емкость: <span><span id="t_cap">--</span> Ач</span></div>
    <div class="stat">Остаток емкости: <span><span id="r_cap">--</span> Ач</span></div>
    <div class="stat">Количество циклов: <span id="cycles">--</span></div>
    <div class="stat">Всего заряжено: <span><span id="tot_ch">--</span> кВт&middot;ч</span></div>
    <div class="stat">Всего разряжено: <span><span id="tot_dis">--</span> кВт&middot;ч</span></div>
    <div class="stat">Зарядный MOSFET: <span id="mosfet_ch">--</span></div>
    <div class="stat">Разрядный MOSFET: <span id="mosfet_disch">--</span></div>
    <div class="stat">Статус балансира: <span id="bal_stat">--</span></div>
    <div class="stat">Флаги инвертору (0x92): <span id="cd_status">--</span></div>
    <div class="stat">Статус (Pylontech): <span id="status">--</span></div>
    <div id="manual_warning"></div>
  </div>

  <h2>Журнал запросов Инвертора</h2>
  <div class="card">
    <div class="log-box" id="inv_log">Ожидание команд от инвертора...</div>
  </div>

  <h2>Ручное управление</h2>
  <div class="card">
    <form action="/toggle_fc" method="POST">
      <input type="submit" id="fc_btn" value="Загрузка..." style="background: #888; color: #fff;">
    </form>
  </div>

  <h2>Ячейки и Температура</h2>
  <div class="card">
    <div style="font-weight: bold; margin-bottom: 5px;">Напряжения ячеек:</div>
    <div class="badge-grid" id="cells_html">Загрузка...</div>
    <div style="font-weight: bold; margin-top: 15px; margin-bottom: 5px;">Датчики температуры:</div>
    <div class="badge-grid" id="temps_html">Загрузка...</div>
  </div>

  <h2>Системные Настройки</h2>
  <form action="/save" method="POST" class="card">
    <h3>Связь MQTT</h3>
    <label>IP-адрес брокера:</label>
    <input type="text" name="mqtt_ip" id="mqtt_ip">
    <label>Префикс топиков (например, antesp32/BMS/):</label>
    <input type="text" name="mqtt_topic" id="mqtt_topic">

    <h3 style="margin-top: 25px;">Лимиты Инвертора</h3>
    <label>Лимит напряжения заряда (мВ):</label>
    <input type="number" name="cv_lim" id="cv_lim">
    <label>Лимит напряжения разряда (мВ):</label>
    <input type="number" name="dv_lim" id="dv_lim">
    <label>Лимит тока заряда (в 100 мА, 1000 = 100А):</label>
    <input type="number" name="cc_lim" id="cc_lim">
    <label>Лимит тока разряда (Отрицательный, в 100 мА):</label>
    <input type="number" name="dc_lim" id="dc_lim">
    
    <h3 style="margin-top: 25px; color: #28a745;">Условия Остановки Заряда</h3>
    <label style="color: #28a745;">Остановить заряд от инвертора при SoC (%):</label>
    <input type="number" name="max_soc" id="max_soc" max="100" min="10">

    <h3 style="margin-top: 25px; color: #d32f2f;">Условия Force Charge</h3>
    <label style="color: #d32f2f;">Включить заряд от сети при SoC ниже (%):</label>
    <input type="number" name="fc_soc" id="fc_soc">
    <label style="color: #d32f2f;">Включить заряд при просадке ячейки ниже (мВ):</label>
    <input type="number" name="fc_cell" id="fc_cell">
    
    <input type="submit" value="Сохранить настройки">
    <div style="text-align: center; margin-top: 20px;">
      <a href="/update" style="color: #0056b3; font-weight: bold; text-decoration: underline;">Обновление прошивки (Web OTA)</a>
    </div>
  </form>

  <script>
    function updateData() {
      var xhr = new XMLHttpRequest();
      xhr.open('GET', '/api/data', true);
      xhr.onload = function() {
        if(xhr.status == 200) {
          var data = JSON.parse(xhr.responseText);
          document.getElementById('v_tot').innerHTML = data.v_tot;
          document.getElementById('i_cur').innerHTML = data.i_cur;
          document.getElementById('soc').innerHTML = data.soc;
          document.getElementById('t_cap').innerHTML = data.t_cap;
          document.getElementById('r_cap').innerHTML = data.r_cap;
          document.getElementById('cycles').innerHTML = data.cycles;
          document.getElementById('tot_ch').innerHTML = data.tot_ch;
          document.getElementById('tot_dis').innerHTML = data.tot_dis;
          document.getElementById('pow').innerHTML = data.pow;
          
          document.getElementById('mosfet_ch').innerHTML = data.mosfet_ch;
          document.getElementById('mosfet_disch').innerHTML = data.mosfet_disch;
          document.getElementById('bal_stat').innerHTML = data.bal_stat;
          document.getElementById('cd_status').innerHTML = data.cd_status;
          document.getElementById('status').innerHTML = data.status;
          document.getElementById('manual_warning').innerHTML = data.manual_warning;
          
          document.getElementById('fc_btn').value = data.fc_btn_text;
          document.getElementById('fc_btn').style.background = data.fc_btn_color;
          
          document.getElementById('inv_log').innerHTML = data.inv_log;
          document.getElementById('cells_html').innerHTML = data.cells_html;
          document.getElementById('temps_html').innerHTML = data.temps_html;

          if(!window.inputsLoaded) {
            document.getElementById('mqtt_ip').value = data.mqtt_ip;
            document.getElementById('mqtt_topic').value = data.mqtt_topic;
            document.getElementById('cv_lim').value = data.cv_lim;
            document.getElementById('dv_lim').value = data.dv_lim;
            document.getElementById('cc_lim').value = data.cc_lim;
            document.getElementById('dc_lim').value = data.dc_lim;
            document.getElementById('max_soc').value = data.max_soc;
            document.getElementById('fc_soc').value = data.fc_soc;
            document.getElementById('fc_cell').value = data.fc_cell;
            window.inputsLoaded = true;
          }
        }
      };
      xhr.send();
    }
    updateData(); 
    setInterval(updateData, 2000); 
  </script>
</body>
</html>
)rawliteral";

// --- ВЕБ-СТРАНИЦА ДЛЯ ЗАГРУЗКИ ПРОШИВКИ (UPDATE HTML) ---
const char UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BMS Gateway - Web OTA</title>
  <style>
    body { font-family: Arial, sans-serif; background: #f4f4f9; padding: 20px; color: #333; text-align: center; }
    .card { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); max-width: 400px; margin: 40px auto; }
    h2 { color: #0056b3; }
    input[type="file"] { margin: 20px 0; width: 100%; border: 1px solid #ccc; padding: 10px; border-radius: 4px; box-sizing: border-box; background: #fff;}
    input[type="submit"] { background: #007bff; color: white; border: none; padding: 10px 20px; font-size: 16px; border-radius: 4px; cursor: pointer; width: 100%; }
    input[type="submit"]:hover { opacity: 0.9; }
    .back-link { display: block; margin-top: 20px; color: #666; text-decoration: none; font-size: 14px; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Обновление прошивки (Web OTA)</h2>
    <p>Выберите скомпилированный файл прошивки (.bin)</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" accept=".bin">
      <input type="submit" value="Загрузить и обновить">
    </form>
    <a href="/" class="back-link">&larr; Назад на главную</a>
  </div>
</body>
</html>
)rawliteral";

void loadSettings() {
  preferences.begin("bms", true); 
  config.charge_voltage_limit = preferences.getInt("cv_lim", 56800);
  config.discharge_voltage_limit = preferences.getInt("dv_lim", 47500);
  config.charge_current_limit = preferences.getInt("cc_lim", 1000);
  config.discharge_current_limit = preferences.getInt("dc_lim", -1000); 
  config.fc_soc_threshold = preferences.getInt("fc_soc", 10);
  config.fc_cell_threshold = preferences.getInt("fc_cell", 2800);
  
  config.mqtt_ip = preferences.getString("mqtt_ip", "192.168.1.100");
  config.mqtt_topic = preferences.getString("mqtt_topic", "antesp32/BMS/");
  
  config.total_charge_kwh = preferences.getDouble("ch_kwh", 0.0);
  config.total_discharge_kwh = preferences.getDouble("dis_kwh", 0.0);

  config.max_soc_threshold = preferences.getInt("max_soc", 100);

  preferences.end();
  for(int i=0; i<NUM_CELLS; i++) config.voltages[i] = 3300;
}

void saveSettings() {
  preferences.begin("bms", false); 
  preferences.putInt("cv_lim", config.charge_voltage_limit);
  preferences.putInt("dv_lim", config.discharge_voltage_limit);
  preferences.putInt("cc_lim", config.charge_current_limit);
  preferences.putInt("dc_lim", config.discharge_current_limit);
  preferences.putInt("fc_soc", config.fc_soc_threshold);
  preferences.putInt("fc_cell", config.fc_cell_threshold);
  
  preferences.putDouble("ch_kwh", config.total_charge_kwh);
  preferences.putDouble("dis_kwh", config.total_discharge_kwh);

  preferences.putString("mqtt_ip", config.mqtt_ip);
  preferences.putString("mqtt_topic", config.mqtt_topic);
  
  preferences.putInt("max_soc", config.max_soc_threshold);

  preferences.end();
}

// Отдача сырого JSON для веб-страницы (работает молниеносно, без нагрузки)
void handleApiData() {
  String json = "{";
  json += "\"v_tot\":\"" + String(config.total_voltage / 1000.0, 2) + "\",";
  json += "\"i_cur\":\"" + String(config.current / -10.0, 1) + "\",";
  json += "\"soc\":\"" + String(config.soc) + "\",";
  json += "\"t_cap\":\"" + String(config.real_total_capacity / 1000.0, 1) + "\",";
  json += "\"r_cap\":\"" + String(config.real_remaining_capacity / 1000.0, 1) + "\",";
  json += "\"cycles\":\"" + String(config.cycles) + "\",";
  json += "\"tot_ch\":\"" + String(config.total_charge_kwh, 2) + "\",";
  json += "\"tot_dis\":\"" + String(config.total_discharge_kwh, 2) + "\",";
  json += "\"pow\":\"" + String(config.power) + "\",";
  
  String chSt = (config.mosfet_charge_st < 16) ? MOSFET_Charge_St[config.mosfet_charge_st] : "Unknown";
  String dischSt = (config.mosfet_discharge_st < 16) ? MOSFET_Discharge_St[config.mosfet_discharge_st] : "Unknown";
  String balSt = (config.bal_st < 11) ? Bal_St[config.bal_st] : "Unknown";
  
  json += "\"mosfet_ch\":\"" + String((config.mosfet_charge_st == 1) ? "<span style='color: #28a745;'>ON</span>" : "<span class='alert'>" + chSt + "</span>") + "\",";
  json += "\"mosfet_disch\":\"" + String((config.mosfet_discharge_st == 1) ? "<span style='color: #28a745;'>ON</span>" : "<span class='alert'>" + dischSt + "</span>") + "\",";
  json += "\"bal_stat\":\"" + String((config.bal_st == 4) ? "<span style='color: #ff9800;'>ACTIVE</span>" : "<span>" + balSt + "</span>") + "\",";

  bool charge_enable = (config.mosfet_charge_st == 1) && (config.soc < config.max_soc_threshold);
  byte cd_status = 0x00;
  if (charge_enable) cd_status |= (1 << 7);
  if (config.mosfet_discharge_st == 1) cd_status |= (1 << 6);
  if (config.status == STATUS_FORCE_CHARGE) cd_status |= (1 << 5);

  String cdHex = String(cd_status, HEX);
  cdHex.toUpperCase();
  if (cdHex.length() < 2) cdHex = "0" + cdHex;

  String ch_text = charge_enable ? "Да" : ((config.soc >= config.max_soc_threshold) ? "Нет (Лимит SoC)" : "Нет (Блок BMS)");
  String cdStr = "0x" + cdHex + " (Заряд: " + ch_text + ", Разряд: " + ((cd_status & (1 << 6)) ? "Да" : "Нет") + ", Force: " + ((cd_status & (1 << 5)) ? "Да" : "Нет") + ")";
  json += "\"cd_status\":\"" + cdStr + "\",";

  String statTxt = (config.status == STATUS_FORCE_CHARGE) ? "<span class='alert'>FORCE CHARGE АКТИВЕН</span>" : "Норма (0xC0)";
  json += "\"status\":\"" + statTxt + "\",";

  String manualWarning = config.manual_force_charge ? "<div class='manual-alert'>Внимание: Включен РУЧНОЙ режим Force Charge!</div>" : "";
  json += "\"manual_warning\":\"" + manualWarning + "\",";
  
  json += "\"fc_btn_text\":\"" + String(config.manual_force_charge ? "Отключить ручной Force Charge" : "Включить ручной Force Charge") + "\",";
  json += "\"fc_btn_color\":\"" + String(config.manual_force_charge ? "#dc3545" : "#ff9800") + "\",";

  String logHtml = "";
  // Захватываем мьютекс перед чтением массива
  if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
    for (int i = 0; i < 6; i++) {
      if (inverter_log[i] != "") logHtml += inverter_log[i] + "<br>";
    }
    xSemaphoreGive(logMutex); // Отпускаем мьютекс
  }
  if (logHtml == "") logHtml = "<i>Ожидание команд от инвертора...</i>";
  json += "\"inv_log\":\"" + logHtml + "\",";

  String cellsHtml = "";
  for (int i = 0; i < NUM_CELLS; i++) {
    float cellVoltage = config.voltages[i] / 1000.0;
    String colorStyle = (config.voltages[i] > 1000 && config.voltages[i] < config.fc_cell_threshold) ? "color: red;" : "";
    cellsHtml += "<div class='badge'>Яч. " + String(i + 1) + "<span style='" + colorStyle + "'>" + String(cellVoltage, 3) + " В</span></div>";
  }
  json += "\"cells_html\":\"" + cellsHtml + "\",";

  String tempsHtml = "";
  for (int i = 0; i < 5; i++) {
    float tempCelsius = config.temperatures[i] / 10.0;
    tempsHtml += "<div class='badge'>Дат. " + String(i + 1) + "<span>" + String(tempCelsius, 1) + " &deg;C</span></div>";
  }
  json += "\"temps_html\":\"" + tempsHtml + "\",";

  json += "\"mqtt_ip\":\"" + config.mqtt_ip + "\",";
  json += "\"mqtt_topic\":\"" + config.mqtt_topic + "\",";
  json += "\"cv_lim\":\"" + String(config.charge_voltage_limit) + "\",";
  json += "\"dv_lim\":\"" + String(config.discharge_voltage_limit) + "\",";
  json += "\"cc_lim\":\"" + String(config.charge_current_limit) + "\",";
  json += "\"dc_lim\":\"" + String(config.discharge_current_limit) + "\",";
  json += "\"max_soc\":\"" + String(config.max_soc_threshold) + "\",";
  json += "\"fc_soc\":\"" + String(config.fc_soc_threshold) + "\",";
  json += "\"fc_cell\":\"" + String(config.fc_cell_threshold) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleSave() {
  if (server.method() == HTTP_POST) {
    if(server.hasArg("cv_lim")) config.charge_voltage_limit = server.arg("cv_lim").toInt();
    if(server.hasArg("dv_lim")) config.discharge_voltage_limit = server.arg("dv_lim").toInt();
    if(server.hasArg("cc_lim")) config.charge_current_limit = server.arg("cc_lim").toInt();
    if(server.hasArg("dc_lim")) config.discharge_current_limit = server.arg("dc_lim").toInt();
    if(server.hasArg("fc_soc")) config.fc_soc_threshold = server.arg("fc_soc").toInt();
    if(server.hasArg("fc_cell")) config.fc_cell_threshold = server.arg("fc_cell").toInt();
    if(server.hasArg("max_soc")) config.max_soc_threshold = server.arg("max_soc").toInt();
    if(server.hasArg("mqtt_ip")) config.mqtt_ip = server.arg("mqtt_ip");
    if(server.hasArg("mqtt_topic")) {
      config.mqtt_topic = server.arg("mqtt_topic");
      if (!config.mqtt_topic.endsWith("/")) {
        config.mqtt_topic += "/";
      }
    }
    
    saveSettings();
    mqttClient.setServer(config.mqtt_ip.c_str(), MQTT_PORT);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleToggleFC() {
  if (server.method() == HTTP_POST) {
    config.manual_force_charge = !config.manual_force_charge;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

bool MQTT_Reconnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected()) return true;

  unsigned long now = millis();
  if (now - lastMqttReconnectAttempt > 5000) {
    lastMqttReconnectAttempt = now;
    Serial.printf("[MQTT] Подключение к %s... ", config.mqtt_ip.c_str());
    
    bool connected = false;
    if (strlen(MQTT_USER) > 0) {
      connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    } else {
      connected = mqttClient.connect(MQTT_CLIENT_ID);
    }

    if (connected) {
      Serial.println("Успешно!");
      return true;
    } else {
      Serial.print("Ошибка, rc=");
      Serial.println(mqttClient.state());
    }
  }
  return false;
}

// --- ВЫДЕЛЕННЫЙ ПОТОК ДЛЯ ИНВЕРТОРА (МАКСИМАЛЬНЫЙ ПРИОРИТЕТ) ---
void InverterTask(void * parameter) {
  for(;;) {
    while (Serial2.available()) {
      char inChar = (char)Serial2.read();
      pylonInputBuffer += inChar;
      
      if (pylonInputBuffer.length() > 100) pylonInputBuffer = ""; // Защита от мусора

      if (inChar == '\r') {
        int tildeIndex = pylonInputBuffer.indexOf('~');
        if (tildeIndex != -1) {
          String cleanCmd = pylonInputBuffer.substring(tildeIndex); 
          handleInverterCommand(cleanCmd);
        }
        pylonInputBuffer = ""; 
      }
    }
    // Отдаем планировщику 5 мс, чтобы не заблокировать Watchdog таймер
    vTaskDelay(5 / portTICK_PERIOD_MS); 
  }
}

// --- ОТПРАВКА ДАННЫХ В MQTT С ДИНАМИЧЕСКИМ ПРЕФИКСОМ ---
void publishTelemetryToMqtt() {
  if (!mqttClient.connected()) return;

  String prefix = config.mqtt_topic;

  mqttClient.publish((prefix + "SOC").c_str(), String(config.soc).c_str());
  mqttClient.publish((prefix + "BMS_pow").c_str(), String(config.power).c_str());

  if (config.mosfet_charge_st < 16) {
    mqttClient.publish((prefix + "BMS_MOSFET_Ch_St").c_str(), MOSFET_Charge_St[config.mosfet_charge_st]);
  }
  if (config.mosfet_discharge_st < 16) {
    mqttClient.publish((prefix + "BMS_MOSFET_Disch_St").c_str(), MOSFET_Discharge_St[config.mosfet_discharge_st]);
  }
  if (config.bal_st < 11) {
    mqttClient.publish((prefix + "BMS_Bal_St").c_str(), Bal_St[config.bal_st]);
  }

  for (int i = 0; i < NUM_CELLS; i++) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%sBMS_Bal%d", prefix.c_str(), i + 1);
    mqttClient.publish(topic, String((config.bal_mask >> i) & 1).c_str());
  }

  mqttClient.publish((prefix + "BMS_Current").c_str(), String(config.current / -10.0, 1).c_str()); //инвертируем значение для mqtt, так как ранее ANT BMS у меня отрицательный был - заряд.
  mqttClient.publish((prefix + "BMS_V").c_str(), String(config.total_voltage / 1000.0, 1).c_str());
  
  mqttClient.publish((prefix + "cell_avg").c_str(), String(config.cell_avg / 1000.0, 3).c_str());
  mqttClient.publish((prefix + "cell_min").c_str(), String(config.cell_min / 1000.0, 3).c_str());
  mqttClient.publish((prefix + "cell_max").c_str(), String(config.cell_max / 1000.0, 3).c_str());

  for (int i = 0; i < NUM_CELLS; i++) {
    if (config.voltages[i] > 1000) {
      char topic[64];
      snprintf(topic, sizeof(topic), "%scell%d", prefix.c_str(), i + 1);
      mqttClient.publish(topic, String(config.voltages[i] / 1000.0, 3).c_str());
    }
  }

  for (int i = 0; i < 5; i++) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%sTemp%d", prefix.c_str(), i + 1);
    mqttClient.publish(topic, String(config.temperatures[i] / 10.0, 1).c_str());
  }

  float cpuTemp = temperatureRead(); 
  mqttClient.publish((prefix + "CPU_temp").c_str(), String(cpuTemp, 1).c_str());

  mqttClient.publish((prefix + "BAT_cycle_capacity").c_str(), String(config.cycle_capacity / 1000.0, 1).c_str());
  mqttClient.publish((prefix + "BAT_remain_capacity").c_str(), String(config.real_remaining_capacity / 1000.0, 1).c_str());
  mqttClient.publish((prefix + "BAT_total_capacity").c_str(), String(config.real_total_capacity / 1000.0, 1).c_str());
  mqttClient.publish((prefix + "BAT_cycles").c_str(), String(config.cycles).c_str());

  // --- ТОПИКИ НАКОПЛЕННОЙ ЭНЕРГИИ ---
  mqttClient.publish((prefix + "BAT_totalChargeKWh").c_str(), String(config.total_charge_kwh, 3).c_str());
  mqttClient.publish((prefix + "BAT_totalDischargeKWh").c_str(), String(config.total_discharge_kwh, 3).c_str());
}

// --- ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ PROTOCOL ---
uint16_t readUint16(uint8_t* buffer, int offset) { return (buffer[offset] << 8) | buffer[offset + 1]; }
int16_t readInt16(uint8_t* buffer, int offset) { return (int16_t)((buffer[offset] << 8) | buffer[offset + 1]); }
uint32_t readUint32(uint8_t* buffer, int offset) { return ((uint32_t)buffer[offset] << 24) | ((uint32_t)buffer[offset + 1] << 16) | ((uint32_t)buffer[offset + 2] << 8) | buffer[offset + 3]; }
int32_t readInt32(uint8_t* buffer, int offset) { return (int32_t)(((uint32_t)buffer[offset] << 24) | ((uint32_t)buffer[offset + 1] << 16) | ((uint32_t)buffer[offset + 2] << 8) | buffer[offset + 3]); }

String byteToHex(byte b) { String hex = String(b, HEX); hex.toUpperCase(); if (hex.length() < 2) hex = "0" + hex; return hex; }

byte calcLchksum(int len) {
  if (len == 0) return 0;
  int l = len * 2;
  int ll = ((l >> 8) & 0xF) + ((l >> 4) & 0xF) + (l & 0xF);
  return (byte)((~(ll % 16) + 1) & 0xF);
}

void addShortToInfo(byte* info, int &index, int16_t val) {
  info[index++] = (val >> 8) & 0xFF;
  info[index++] = val & 0xFF;
}

void assembleAndSendPylonResponse(byte ver, byte adr, byte cid1, byte cid2, byte* info, int infoLen) {
  int totalBytes = 4 + 2 + infoLen;
  byte* msgBytes = new byte[totalBytes];
  msgBytes[0] = ver; msgBytes[1] = adr; msgBytes[2] = cid1; msgBytes[3] = cid2;
  
  byte lchksum = calcLchksum(infoLen);
  int l_ascii_len = infoLen * 2;
  msgBytes[4] = (lchksum << 4) | ((l_ascii_len >> 8) & 0xF);
  msgBytes[5] = l_ascii_len & 0xFF;
  for(int i = 0; i < infoLen; i++) msgBytes[6 + i] = info[i];
  
  String asciiStr = "";
  for(int i = 0; i < totalBytes; i++) asciiStr += byteToHex(msgBytes[i]);
  long sum = 0;
  for (int i = 0; i < asciiStr.length(); i++) sum += asciiStr.charAt(i);
  unsigned int cs = ((sum ^ 0xFFFF) + 1) & 0xFFFF;
  
  String finalPacket = "~" + asciiStr + byteToHex((cs >> 8) & 0xFF) + byteToHex(cs & 0xFF) + "\r";
  
  //String debugTx = finalPacket;
  //debugTx.replace("\r", ""); 
  //Serial.println("[PYLON TX] " + debugTx);

  digitalWrite(RS485_DE_RE, HIGH);
  delay(2); 
  Serial2.print(finalPacket);
  Serial2.flush(); 
  delay(2);
  digitalWrite(RS485_DE_RE, LOW);
  delete[] msgBytes;
}

void printHex(uint8_t* data, int len) {
  
  Serial.print("[ANT RX] ");
  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  
}

// --- ПАРСИНГ ANT BMS И АВТОМАТИКА СТАТУСОВ ---
void parseAntBmsData() {
  if (memcmp(antBuffer, antHeader, 4) != 0) {
    Serial.println("[ANT ERROR] Неверный заголовок пакета!");
    return; 
  }

  uint16_t calculatedChecksum = 0;
  for (int i = 4; i < 138; i++) calculatedChecksum += antBuffer[i];
  uint16_t receivedChecksum = (antBuffer[138] << 8) | antBuffer[139];
  
  if (calculatedChecksum != receivedChecksum) {
    Serial.printf("[ANT ERROR] Ошибка CRC! Рассчитано: %04X, Получено: %04X\n", calculatedChecksum, receivedChecksum);
    return; 
  }

  bool cellUnderThreshold = false;
  bool allCellsAboveRecovery = true;

  uint16_t antTotalVolts = readUint16(antBuffer, 4);
  config.total_voltage = antTotalVolts * 100; 

  for (int i = 0; i < NUM_CELLS; i++) {
    config.voltages[i] = readUint16(antBuffer, 6 + (i * 2));
    if (config.voltages[i] > 1000) { 
      if (config.voltages[i] < config.fc_cell_threshold) cellUnderThreshold = true;
      if (config.voltages[i] < (config.fc_cell_threshold + 200)) allCellsAboveRecovery = false; 
    }
  }

  int32_t antCurrentRaw = readInt32(antBuffer, 70); 
  float currentAmps = (antCurrentRaw / 10.0) * -1.0; 
  config.current = (int16_t)round(currentAmps * 10.0);
  config.soc = antBuffer[74];
  
  config.power = readInt32(antBuffer, 111);
  config.mosfet_charge_st = antBuffer[103];
  config.mosfet_discharge_st = antBuffer[104];
  config.bal_st = antBuffer[105];
  config.bal_mask = readUint16(antBuffer, 134);
  config.cell_avg = readUint16(antBuffer, 121);
  config.cell_min = readUint16(antBuffer, 119);
  config.cell_max = readUint16(antBuffer, 116);

  uint32_t totalCapRaw = readUint32(antBuffer, 75);
  uint32_t totalCap_mAh = totalCapRaw / 1000; 
  uint32_t remCapRaw = readUint32(antBuffer, 79);
  uint32_t remCap_mAh = remCapRaw / 1000; 

  config.real_total_capacity = totalCap_mAh;
  config.real_remaining_capacity = remCap_mAh;

  if (totalCap_mAh > 65000) {
    // config.total_capacity = 65000; 
    // config.remaining_capacity = (65000 * config.soc) / 100;
    config.total_capacity = totalCap_mAh/10; 
    config.remaining_capacity = remCap_mAh/10; 
  } else {
    config.total_capacity = totalCap_mAh; 
    config.remaining_capacity = remCap_mAh; 
  }

  uint32_t cycleCapRaw = readUint32(antBuffer, 83); 
  config.cycle_capacity = cycleCapRaw ; 
  if (totalCap_mAh > 0) {
    config.cycles = cycleCapRaw / totalCap_mAh; 
  }

  if (config.manual_force_charge) {
    config.status = STATUS_FORCE_CHARGE; 
  } 
  else {
    if (config.soc <= config.fc_soc_threshold || cellUnderThreshold) {
      config.status = STATUS_FORCE_CHARGE; 
    } 
    else if (config.soc >= (config.fc_soc_threshold + 5) && allCellsAboveRecovery) {
      config.status = STATUS_NORMAL;       
    }
  }

  for (int i = 0; i < 5; i++) {
    config.temperatures[i] = readInt16(antBuffer, 91 + (i * 2)) * 10; 
  }

  // --- РАСЧЕТ НАКОПЛЕННОЙ ЭНЕРГИИ (кВт·ч) НА ОСНОВЕ МОЩНОСТИ BMS ---
  static unsigned long lastEnergyCalcTime = 0;
  static double lastSavedCharge = 0;
  static double lastSavedDischarge = 0;
  static unsigned long lastSavePeriodTime = 0;
  
  unsigned long currentTime = millis();
  
  if (lastEnergyCalcTime == 0) {
    lastEnergyCalcTime = currentTime;
    lastSavedCharge = config.total_charge_kwh;
    lastSavedDischarge = config.total_discharge_kwh;
    lastSavePeriodTime = currentTime;
    return;
  }
  
  // Вычисляем прошедшее время в часах
  double deltaHours = (currentTime - lastEnergyCalcTime) / 3600000.0;
  lastEnergyCalcTime = currentTime;
  
  // Берем чистую мощность в Ваттах напрямую из BMS (используем abs на случай отрицательных значений при разряде)
  double powerW = abs((double)config.power); 
  
  // Энергия за этот шаг (в кВт·ч)
  double energyKWh = (powerW * deltaHours) / 1000.0;
  
  // Распределяем по направлениям, используя мощности (он у нас четко привязан к заряду/разряду)
  if (config.power < 0) {
    config.total_charge_kwh += energyKWh;
  } else if (config.power > 0) {
    config.total_discharge_kwh += energyKWh;
  }
  
  // ЗАЩИТА ФЛЕШ-ПАМЯТИ: сохраняем только при изменении на 0.5 кВт·ч или раз в 5 минут
  if ((config.total_charge_kwh - lastSavedCharge >= 0.5) || 
      (config.total_discharge_kwh - lastSavedDischarge >= 0.5) || 
      (currentTime - lastSavePeriodTime > 300000)) {
        
    saveSettings();
    lastSavedCharge = config.total_charge_kwh;
    lastSavedDischarge = config.total_discharge_kwh;
    lastSavePeriodTime = currentTime;
    Serial.println("[SYSTEM] Счетчики энергии сохранены в Flash-память.");
  }
}

// --- ЛОГИРОВАНИЕ КОМАНД ---
void addInverterLog(byte cid2) {
  String cmdName = "";
  switch(cid2) {
    case 0x42: cmdName = "0x42 (Аналоговые данные)"; break;
    case 0x44: cmdName = "0x44 (Аварии и статусы)"; break;
    case 0x47: cmdName = "0x47 (Системные параметры)"; break;
    case 0x4F: cmdName = "0x4F (Версия протокола)"; break;
    case 0x51: cmdName = "0x51 (Информация о системе)"; break;
    case 0x92: cmdName = "0x92 (Управление зарядом)"; break;
    case 0x93: cmdName = "0x93 (Серийный номер)"; break;
    case 0x96: cmdName = "0x96 (Версия прошивки)"; break;
    default:   cmdName = "0x" + String(cid2, HEX) + " (Неизвестная)"; break;
  }
  
  unsigned long upSec = millis() / 1000;
  String timeStr = String(upSec / 3600) + "ч " + String((upSec % 3600) / 60) + "м " + String(upSec % 60) + "с";
  String entry = "<span style='color: #0056b3;'>[" + timeStr + "]</span> Запрос: <b>" + cmdName + "</b>";

  // Захватываем мьютекс перед записью в массив
  if (xSemaphoreTake(logMutex, portMAX_DELAY)) {
    for (int i = 5; i > 0; i--) inverter_log[i] = inverter_log[i - 1];
    inverter_log[0] = entry;
    xSemaphoreGive(logMutex); // Отпускаем мьютекс
  }
}

// --- ОБРАБОТКА КОМАНД ИНВЕРТОРА ---
void handleInverterCommand(String cmd) {
 // String debugRx = cmd;
// debugRx.replace("\r", ""); 
  //Serial.println("[PYLON RX] " + debugRx);

  cmd.replace("~", "");
  cmd.trim();
  if (cmd.length() < 8) return;
  
  byte adr = strtol(cmd.substring(2, 4).c_str(), NULL, 16);
  byte cid2 = strtol(cmd.substring(6, 8).c_str(), NULL, 16);
  if (adr != 0x02) return; 
  
  addInverterLog(cid2); // Записываем в Web-лог

 if (cid2 == 0x4F) { 
    // Команда 0x4F (Get protocol version)
    // Блок данных пустой (NULL, 0). Инвертор смотрит на байт VER в самом заголовке.
    // Согласно документации, передаем V2.0 (0x20) первым аргументом.
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, NULL, 0);
  }
  else if (cid2 == 0x42) { 
    byte info[70];
    int idx = 0;
    info[idx++] = 0x00; 
    info[idx++] = 0x01; 
    info[idx++] = NUM_CELLS-1; //почему то мой инвертер не работает когда 16 ячеек, поэтому передаю 15
    for(int i=0; i<NUM_CELLS-1; i++) addShortToInfo(info, idx, config.voltages[i]);
    info[idx++] = 5;  
    for(int i=0; i<5; i++) addShortToInfo(info, idx, config.temperatures[i] + 2731); 
    addShortToInfo(info, idx, config.current); 
    addShortToInfo(info, idx, config.total_voltage);
    addShortToInfo(info, idx, config.remaining_capacity);
    info[idx++] = 0x00; 
    addShortToInfo(info, idx, config.total_capacity);
    addShortToInfo(info, idx, config.cycles);
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  } 
  // else if (cid2 == 0x44) { 
  //   byte info[50];
  //   int idx = 0;
  //   info[idx++] = 0x00; 
  //   info[idx++] = 0x01; 
  //   info[idx++] = NUM_CELLS;
  //   for(int i = 0; i < NUM_CELLS; i++) {
  //     if (config.voltages[i] > 1000 && config.voltages[i] < config.fc_cell_threshold) info[idx++] = 0x01; 
  //     else if (config.voltages[i] > (config.charge_voltage_limit / NUM_CELLS) + 100) info[idx++] = 0x02; 
  //     else info[idx++] = 0x00; 
  //   }
  //   info[idx++] = 5;
  //   for(int i = 0; i < 5; i++) {
  //     if (config.temperatures[i] < 0) info[idx++] = 0x01; 
  //     else if (config.temperatures[i] > 550) info[idx++] = 0x02; 
  //     else info[idx++] = 0x00; 
  //   }
  //   info[idx++] = (config.current > config.charge_current_limit) ? 0x02 : 0x00; 
  //   info[idx++] = (config.total_voltage > config.charge_voltage_limit) ? 0x02 : ((config.total_voltage < config.discharge_voltage_limit) ? 0x01 : 0x00); 
  //   info[idx++] = (config.current < config.discharge_current_limit) ? 0x02 : 0x00; 
  //   info[idx++] = (config.soc <= config.fc_soc_threshold) ? 0x01 : 0x00; 
  //   info[idx++] = 0x00; 
  //   info[idx++] = 0x00;
  //   info[idx++] = 0x00;
  //   info[idx++] = 0x00;
  //   assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  // }
  else if (cid2 == 0x44) { //доработана из версии 3.8 протокола
    byte info[50];
    int idx = 0;
    
    // INFO = DATAFLAG + WARNSTATE (Command value + Module alarm info)
    info[idx++] = 0x00; // DATAFLAG (0x00 = нормальный ответ)
    info[idx++] = 0x01; // Command value (Эмулируем ответ от 1-й сборки)
    
    info[idx++] = NUM_CELLS; // number of cell: M 
    
    bool cell_uv = false;
    bool cell_ov = false;
    byte status4 = 0x00;
    byte status5 = 0x00;
    
    for(int i = 0; i < NUM_CELLS; i++) {
      // Базовые алармы для каждой ячейки (00H - норма, 01H - ниже нормы, 02H - выше нормы)
      if (config.voltages[i] > 1000 && config.voltages[i] < config.fc_cell_threshold) {
        info[idx++] = 0x01; 
        cell_uv = true;
      }
      else if (config.voltages[i] > (config.charge_voltage_limit / NUM_CELLS) + 100) {
        info[idx++] = 0x02; 
        cell_ov = true;
      }
      else {
        info[idx++] = 0x00; 
      }
      
      // Критические ошибки для Status 4 и 5 (согласно документации: >4.2V или <1.0V)
      if (config.voltages[i] > 4200 || config.voltages[i] < 1000) {
         if (i < 8) {
            status4 |= (1 << i);       // Ячейки 1-8
         } else {
            status5 |= (1 << (i - 8)); // Ячейки 9-16
         }
      }
    }
    
    info[idx++] = 5; // number of temperature: N
    
    bool charge_ot = false;
    bool discharge_ot = false;
    
    for(int i = 0; i < 5; i++) {
      if (config.temperatures[i] < 0) {
        info[idx++] = 0x01; // Переохлаждение
      } 
      else if (config.temperatures[i] > 550) {
        info[idx++] = 0x02; // Перегрев (> 55°C)
        if (config.current > 0) charge_ot = true;
        if (config.current < 0) discharge_ot = true;
      } 
      else {
        info[idx++] = 0x00; 
      }
    }
    
    // Charge current alarm
    bool coc = (config.current > config.charge_current_limit);
    info[idx++] = coc ? 0x02 : 0x00; 
    
    // Module voltage alarm
    bool mod_uv = (config.total_voltage < config.discharge_voltage_limit);
    bool mod_ov = (config.total_voltage > config.charge_voltage_limit);
    info[idx++] = mod_ov ? 0x02 : (mod_uv ? 0x01 : 0x00); 
    
    // Discharge current alarm (оба значения отрицательные, например -150A < -100A)
    bool doc = (config.current < config.discharge_current_limit);
    info[idx++] = doc ? 0x02 : 0x00; 
    
    // --- СБОРКА БИТОВЫХ МАСОК (Status 1 - 5) ---
    
    // Status 1 (UV, OV, Перегрев, Перегрузка)
    byte status1 = 0x00;
    if (mod_uv) status1 |= (1 << 7);
    if (charge_ot) status1 |= (1 << 6);
    if (discharge_ot) status1 |= (1 << 5);
    if (doc) status1 |= (1 << 4);
    if (coc) status1 |= (1 << 2);
    if (cell_uv) status1 |= (1 << 1);
    if (mod_ov) status1 |= (1 << 0);
    info[idx++] = status1;
    
    // Status 2 (Использование батареи и состояние MOSFET)
    byte status2 = 0x00;
    status2 |= (1 << 3); // 1: using battery module power
    // Берем реальный статус реле из ANT BMS (где "1" = "ON")
    if (config.mosfet_discharge_st == 1) status2 |= (1 << 2); // Discharge MOSFET
    if (config.mosfet_charge_st == 1) status2 |= (1 << 1);    // Charge MOSFET
    info[idx++] = status2;
    
    // Status 3 (Эффективные токи и заряд)
byte status3 = 0x00;
    if (config.current >= 1) status3 |= (1 << 7); 
    if (config.current <= -1) status3 |= (1 << 6); 
    // Если SoC достиг нашего лимита, говорим инвертору, что мы полны (100%)
    if (config.soc >= config.max_soc_threshold || config.soc == 100) status3 |= (1 << 3); 
    info[idx++] = status3;
    
    // Status 4 (Критические ошибки ячеек 1-8)
    info[idx++] = status4;
    
    // Status 5 (Критические ошибки ячеек 9-16)
    info[idx++] = status5;
    
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }
  // else if (cid2 == 0x92) { 
  //   byte info[20];
  //   int idx = 0;
  //   info[idx++] = 0x02;
  //   addShortToInfo(info, idx, config.charge_voltage_limit);
  //   addShortToInfo(info, idx, config.discharge_voltage_limit);
  //   addShortToInfo(info, idx, config.charge_current_limit);
  //   addShortToInfo(info, idx, abs(config.discharge_current_limit)); 
  //   info[idx++] = config.status; 
  //   assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  // }
  else if (cid2 == 0x92) { //доработка из документа версии 3.8
    // Команда 0x92 (Get charge and discharge management info)
    // 1 байт команды + 4*2 байта лимитов + 1 байт статуса = 10 байт
    byte info[10];
    int idx = 0;
    // 1. Command value (0x02 - данные для 2-й батареи)
    info[idx++] = 0x02; 
    // 2. Charge voltage limit (в мВ)
    addShortToInfo(info, idx, config.charge_voltage_limit);
    // 3. Discharge voltage limit (в мВ)
    addShortToInfo(info, idx, config.discharge_voltage_limit);
    // 4. Charge current limit (в 0.1 А)
    // Динамический лимит тока
    int dynamic_charge_current = (config.soc >= config.max_soc_threshold) ? 0 : config.charge_current_limit;
    addShortToInfo(info, idx, dynamic_charge_current);
    // 5. Discharge current limit (в 0.1 А, Абсолютное значение без знака минус)
    addShortToInfo(info, idx, abs(config.discharge_current_limit)); 
    // 6. Charge, discharge status (1 byte)
    byte cd_status = 0x00;
    // Заряд разрешен, только если MOSFET открыт И мы еще не достигли лимита SoC
    bool charge_enable = (config.mosfet_charge_st == 1) && (config.soc < config.max_soc_threshold);
    if (charge_enable) cd_status |= (1 << 7);
    if (config.mosfet_discharge_st == 1) cd_status |= (1 << 6);
    if (config.status == STATUS_FORCE_CHARGE) cd_status |= (1 << 5);
    info[idx++] = cd_status;
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }
  else if (cid2 == 0x47) { 
    // INFOFLAG (1 байт) + 12 параметров * 2 байта = 25 байт
    byte info[25]; 
    int idx = 0;
    info[idx++] = 0x00; // INFOFLAG
    // 1. cell high voltage limit (Верхний лимит ячейки)
    addShortToInfo(info, idx, config.charge_voltage_limit / NUM_CELLS);
        // 2. cell low voltage limit (Нижний лимит ячейки - Тревога)
    addShortToInfo(info, idx, config.fc_cell_threshold + 100);
        // 3. cell under voltage limit (Нижний лимит ячейки - Защита/Отключение)
    addShortToInfo(info, idx, config.fc_cell_threshold);
        // 4. charge high temperature limit (Заряд Макс: 55°C -> 550 + 2731 = 3281)
    addShortToInfo(info, idx, 3281);
        // 5. charge low temperature limit (Заряд Мин: -30°C -> -300 + 2731 = 2431)
    addShortToInfo(info, idx, 2431);
        // 6. charge current limit (Ток заряда, 1 ед = 100mA)
    // Динамический лимит тока (если SoC больше макс. лимита, ставим 0А)
    int dynamic_charge_current = (config.soc >= config.max_soc_threshold) ? 0 : config.charge_current_limit;
    addShortToInfo(info, idx, dynamic_charge_current);
        // 7. module high voltage limit (Верхний лимит всей сборки)
    addShortToInfo(info, idx, config.charge_voltage_limit);
        // 8. module low voltage limit (Нижний лимит сборки - Тревога, делаем на 1В выше защиты)
    addShortToInfo(info, idx, config.discharge_voltage_limit + 1000);
       // 9. module under voltage limit (Нижний лимит сборки - Защита)
    addShortToInfo(info, idx, config.discharge_voltage_limit);
        // 10. discharge high temperature limit (Разряд Макс: 60°C -> 600 + 2731 = 3331)
    addShortToInfo(info, idx, 3331);
        // 11. discharge low temperature limit (Разряд Мин: -20°C -> -200 + 2731 = 2531)
    addShortToInfo(info, idx, 2531);
        // 12. discharge current limit (Ток разряда, "with symbol" - передаем с минусом)
    addShortToInfo(info, idx, config.discharge_current_limit); 
        assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }
  else if (cid2 == 0x51) { 
    // 10 байт (Имя) + 2 байта (Версия) + 20 байт (Производитель) = 32 байта
    byte info[32];
    int idx = 0;
    
    // 1. battery name (10 bytes ASCII)
    // Маскируемся под классическую сборку US2000, добивая пробелами до 10 символов
    const char* batName = "US2000    "; 
    for(int i = 0; i < 10; i++) info[idx++] = batName[i];
    
    // 2. software version (2 bytes)
    // Передаем версию прошивки 1.0
    info[idx++] = 0x01;
    info[idx++] = 0x00;
    
    // 3. manufacturer name (20 bytes ASCII)
    // Строгие инверторы ищут именно слово Pylontech. Добиваем пробелами до 20 символов
    const char* mfgName = "Pylontech           ";
    for(int i = 0; i < 20; i++) info[idx++] = mfgName[i];
    
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }
  else if (cid2 == 0x96) { 
    // Команда 0x96 (Get software version)
    // Структура: Command value (1) + Manufacture version (2) + Main line version (3) = 6 байт
    byte info[6];
    int idx = 0;
    
    // 1. Command value
    info[idx++] = 0x00; // 0x00 - стандартный код успешного ответа
    
    // 2. Module software version (5 bytes)
    // 2.1 Manufacture version (2 bytes) - передаем "V2.0"
    info[idx++] = 0x02; 
    info[idx++] = 0x00; 
    
    // 2.2 Main line version (3 bytes) - передаем "V1.0.0"
    info[idx++] = 0x01; 
    info[idx++] = 0x00; 
    info[idx++] = 0x00; 
    
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }
  else if (cid2 == 0x93) { 
    // Команда 0x93 (Get module SN number)
    // Структура: Command value (1 байт) + Module SN (16 байт ASCII) = 17 байт
    byte info[17];
    int idx = 0;
    
    // 1. Command value
    info[idx++] = 0x00; // 0x00 - стандартный статус успешного ответа
    
    // 2. Module SN number (ровно 16 символов ASCII)
    const char* sn = "PYLON_ESP32_BMS1"; 
    for(int i = 0; i < 16; i++) {
      info[idx++] = sn[i];
    }
    
    assembleAndSendPylonResponse(0x20, 0x02, 0x46, 0x00, info, idx);
  }



}

void setup() {
  logMutex = xSemaphoreCreateMutex();
  Serial.begin(115200); 
  
  Serial1.setRxBufferSize(256);
  Serial1.setTxBufferSize(256);
  Serial1.begin(19200, SERIAL_8N1, RXD1, TXD1);  
  
  Serial2.setRxBufferSize(512);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);  
  
  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW); 

  loadSettings();

  Serial.print("Подключение к Wi-Fi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Успешно! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[Wi-Fi ERROR] Нет сети, запуск в автономном режиме.");
  }

  // Применяем IP брокера из сохраненных настроек
  mqttClient.setServer(config.mqtt_ip.c_str(), MQTT_PORT);
  
  // ВЕБ СЕРВЕР (Использует легкую AJAX-архитектуру)
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/data", HTTP_GET, handleApiData); // <--- ТОЧКА ВХОДА AJAX
  server.on("/save", HTTP_POST, handleSave);
  server.on("/toggle_fc", HTTP_POST, handleToggleFC);
  // ---> ДОБАВЛЕНЫ ОБРАБОТЧИКИ WEB OTA <---
  // 1. Показ страницы загрузки
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html", String(UPDATE_HTML));
  });
  
  // 2. Обработчик финализации POST-запроса (успех/ошибка)
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      server.send(200, "text/html", "<h3>Ошибка обновления! Файл поврежден.</h3><br><a href='/update'>Назад</a>");
    } else {
      server.send(200, "text/html", "<h3>Обновление успешно завершено! Плата перезагружается...</h3><script>setTimeout(function(){location.href='/';}, 5000);</script>");
      delay(1000);
      ESP.restart(); 
    }
  }, []() {
    // 3. Лямбда-функция для побайтового приема самого файла (Upload handler)
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Web OTA Начало: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { 
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { 
        Serial.printf("Web OTA Успешно: %u байт залито.\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });
  // ----------------------------------------

  server.begin();
  Serial.println("HTTP сервер запущен");

// Запуск выделенного потока для инвертора
  xTaskCreatePinnedToCore(
    InverterTask,       // Имя функции
    "Inverter_Task",    // Системное имя задачи
    8192,               // Размер выделенного стека (с запасом)
    NULL,               // Параметры
    5,                  // Высший приоритет (у стандартного loop() приоритет 1)
    &InverterTaskHandle,// Указатель на дескриптор
    1                   // Выполнять на ядре 1 (на ядре 0 живет радиомодуль Wi-Fi)
  );
}

void loop() {
  server.handleClient();
  unsigned long currentMillis = millis();

  static unsigned long lastWifiCheck = 0;
  if (currentMillis - lastWifiCheck > 30000) {
    lastWifiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (MQTT_Reconnect()) {
      mqttClient.loop();
    }
  }

// === НАЧАЛО НОВОГО БЛОКА ANT BMS ===

// 1. Отправляем запрос BMS
  if (currentMillis - lastBmsRequestTime >= bmsInterval) {
    lastBmsRequestTime = currentMillis;
    Serial1.write(antRequest, 6); 
  }

  // 2. АСИНХРОННОЕ ЧТЕНИЕ ANT BMS (БЕЗ БЛОКИРОВОК И ЗАДЕРЖЕК CPU)
  static int antRxIndex = 0;
  static unsigned long lastAntRxTime = 0;

  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    lastAntRxTime = millis();

    if (antRxIndex == 0 && b != 0xAA) continue;
    if (antRxIndex == 1 && b != 0x55) { antRxIndex = 0; continue; }
    if (antRxIndex == 2 && b != 0xAA) { antRxIndex = 0; continue; }
    if (antRxIndex == 3 && b != 0xFF) { antRxIndex = 0; continue; }

    antBuffer[antRxIndex++] = b;

    if (antRxIndex == ANT_PACKET_SIZE) {
      parseAntBmsData(); 
      antRxIndex = 0;
    }
  }
  // Сбрасываем "битый" пакет, если следующий байт не пришел за 100 мс
  if (antRxIndex > 0 && millis() - lastAntRxTime > 100) antRxIndex = 0;

  // === КОНЕЦ НОВОГО БЛОКА ANT BMS ===

  if (currentMillis - lastMqttPublishTime >= mqttPublishInterval) {
    lastMqttPublishTime = currentMillis;
    publishTelemetryToMqtt();
  }
}
