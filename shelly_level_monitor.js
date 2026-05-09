// Shelly script to monitor hot tub water level and control water supply
// Checks every 1 minute if level is below 100%, turns OFF device if not at 100%

let config = {
  hottub_ip: "192.168.10.85",
  check_interval_ms: 60000,  // 1 minute
  relay_id: 1,               // Which relay controls the water supply (Shelly Pro 4PM Relay-1)
};

function checkLevel() {
  let url = "http://" + config.hottub_ip + "/api/status";
  
  Shelly.call(
    "HTTP.GET",
    { url: url },
    function (result, error_code, error_msg) {
      if (error_code === 0) {
        try {
          let data = JSON.parse(result.body);
          let level = data.level_percent;
          
          print("Water level: " + level + "%");
          
          // Turn OFF if level is below 100% (water supply gate)
          if (level < 100) {
            print("Level below 100%, turning OFF water supply");
            Shelly.call("Switch.Set", { id: config.relay_id, on: false });
          } else {
            print("Level at 100%, water supply can be ON if needed");
          }
        } catch (e) {
          print("Error parsing response: " + e);
        }
      } else {
        print("HTTP request failed: " + error_msg);
      }
    }
  );
}

// Start periodic check
print("Starting water level monitor...");
Timer.set(config.check_interval_ms, true, checkLevel);
