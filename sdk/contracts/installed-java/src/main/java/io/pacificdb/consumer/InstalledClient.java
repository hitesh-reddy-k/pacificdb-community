package io.pacificdb.consumer;
import io.pacificdb.*;
import java.util.Map;
/** Compiles and runs against the installed Maven coordinate, never SDK class directories. */
public final class InstalledClient {
    public static void main(String[] args)throws Exception {
        try(var db=PacificDB.fromUrl("pacificdb://localhost:9000/app")) {
            if(!db.getDatabase().equals("app")||db.media()==null||db.backups()==null||db.indexes()==null||db.security()==null)throw new AssertionError();
            if(!PacificDBClient.class.getProtectionDomain().getCodeSource().getLocation().toString().endsWith("pacificdb-client-1.1.1.jar"))throw new AssertionError("Source classes used instead of installed JAR");
        }
        if(System.getenv("PACIFICDB_URL")!=null)try(var db=PacificDB.connect(System.getenv("PACIFICDB_URL"))) {
            db.createDatabase();db.createCollection("users");db.insert("users",Map.of("id","1","name","Ada"));
            if(!db.findOne("users",Map.of("id","1")).get("name").equals("Ada"))throw new AssertionError();
            db.updateOne("users",Map.of("id","1"),Map.of("$set",Map.of("name","Grace")));
            if(!db.findOne("users",Map.of("id","1")).get("name").equals("Grace"))throw new AssertionError();
            db.deleteOne("users",Map.of("id","1"));if(db.findOne("users",Map.of("id","1"))!=null)throw new AssertionError();
        }
        System.out.println("PASS: separate installed Maven consumer");
    }
}
