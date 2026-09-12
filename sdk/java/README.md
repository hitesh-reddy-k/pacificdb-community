<p align="center">
  <img src="https://raw.githubusercontent.com/hitesh-reddy-k/pacificdb-community/main/site/pacificdb-logo.png" width="112" alt="PacificDB logo">
</p>

# PacificDB Java client

Apache-2.0 client for the Community engine JSON protocol.

```java
var db = new PacificDBClient("127.0.0.1", 9000, "app");
var status = db.request(Map.of("action", "ping"));
```
