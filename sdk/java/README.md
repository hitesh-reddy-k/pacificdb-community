# PacificDB Java client

Apache-2.0 client for the Community engine JSON protocol.

```java
var db = new PacificDBClient("127.0.0.1", 9000, "app");
var status = db.request(Map.of("action", "ping"));
```
