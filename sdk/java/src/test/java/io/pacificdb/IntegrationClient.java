package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.security.MessageDigest;

/** Real-engine qualification, invoked by the disposable Node controller. */
public final class IntegrationClient {
    private static final ObjectMapper JSON=new ObjectMapper();
    @SuppressWarnings("unchecked") private static Map<String,Object> map(Object v){return (Map<String,Object>)v;}
    private static void check(boolean v){if(!v)throw new AssertionError("SDK semantic check failed");}
    private static void refused(Runnable operation){try{operation.run();}catch(PacificDBException error){return;}throw new AssertionError("operation should have been refused");}
    private static String sha(byte[] bytes)throws Exception{StringBuilder out=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes)){out.append(Character.forDigit((b>>4)&15,16));out.append(Character.forDigit(b&15,16));}return out.toString();}
    public static void main(String[] args)throws Exception{
        Path root=Path.of(System.getenv("PACIFICDB_SDK_TEST_ROOT"));String phase=System.getenv("PACIFICDB_SDK_PHASE"),url=System.getenv("PACIFICDB_URL");
        Map<String,Object> options=new HashMap<>(Map.of("timeoutMs",120000));if(System.getenv("PACIFICDB_SDK_CA")!=null)options.put("caFile",System.getenv("PACIFICDB_SDK_CA"));
        List<Map<String,Object>> expected=new ArrayList<>();for(int i=0;i<128;i++){
            Map<String,Object> row=new LinkedHashMap<>(Map.of("id",String.valueOf(i),"value",i,"unicode","నమస్తే","nested",Map.of("enabled",true),"tags",List.of("a","b")));row.put("empty",null);expected.add(row);
        }
        byte[] source=new byte[700000];for(int i=0;i<source.length;i++)source[i]=(byte)(i%251);
        try(PacificDBClient db=PacificDB.connect(url,options)){
            if(phase.equals("prepare")){
                db.createDatabase();db.createCollection("docs");db.createCollection("assets");db.createCollection("vectors");check(db.getDatabase().equals("sdk_java"));check(db.listDatabases().contains("sdk_java"));
                Map<String,Object> inserted=map(db.insertMany("docs",expected));check(((Number)inserted.get("failed")).intValue()==0&&((Number)inserted.get("inserted")).intValue()==128);
                ExecutorService workers=Executors.newFixedThreadPool(8);try{List<Callable<Object>> calls=new ArrayList<>();for(int i=0;i<32;i++){int id=i;calls.add(()->db.insert("docs",Map.of("id","concurrent-"+id,"value",id)));}for(Future<Object> result:workers.invokeAll(calls))result.get();}finally{workers.shutdownNow();}
                check(((Number)map(db.count("docs")).get("count")).intValue()==160);check(((List<?>)map(db.find("docs",Map.of(),5,2)).get("data")).size()==5);
                db.updateOne("docs",Map.of("id","0"),Map.of("$set",Map.of("value",500)));expected.get(0).put("value",500);
                db.updateMany("docs",Map.of("id","1"),Map.of("$set",Map.of("value",501)));expected.get(1).put("value",501);
                Map<String,Object> bulk=map(db.bulkWrite("docs",List.of(Map.of("action","insertOne","data",Map.of("id","temporary","value",0)),Map.of("action","updateOne","filter",Map.of("id","temporary"),"update",Map.of("value",1)),Map.of("action","deleteOne","filter",Map.of("id","temporary")))));check(((Number)bulk.get("failed")).intValue()==0);check(db.findOne("docs",Map.of("id","temporary"))==null);
                db.insert("docs",Map.of("id","delete-many"));db.deleteMany("docs",Map.of("id","delete-many"));
                check(map(db.aggregate("docs",List.of(Map.of("$match",Map.of("id","0")),Map.of("$count","total")))).get("data").equals(List.of(Map.of("total",1))));check(map(db.explain("docs",Map.of("id","0"))).get("status").equals("ok"));
                db.indexes().create(Map.of("collection","docs","name","by_value","fields",Map.of("value",1)));check(map(db.indexes().list(Map.of("collection","docs"))).get("status").equals("ok"));check(db.indexes().validate(Map.of("collection","docs"))!=null);
                db.vectors().insert(Map.of("collection","vectors","data",Map.of("id","east","vector",List.of(1,0))));check(map(((List<?>)map(db.vectors().query(Map.of("collection","vectors","vector",List.of(1,0),"k",1))).get("data")).get(0)).get("id").equals("east"));
                db.media().put("assets","tiny",new byte[]{0,(byte)255},Map.of("contentType","application/octet-stream"));check(Arrays.equals(new byte[]{0,(byte)255},(byte[])db.media().get("assets","tiny").get("data")));
                Path input=root.resolve("java-source.bin");Files.write(input,source);Map<String,Object> media=db.media().uploadFile("assets",input,null,65536,null);check(db.media().uploadFile("assets",input,null,65536,(String)media.get("id")).get("id").equals(media.get("id")));
                refused(()->db.useDatabase("missing_database"));refused(()->db.createDatabase("../unsafe"));check(db.getDatabase().equals("sdk_java"));
                Map<String,Object> project=map(map(db.admin().communityProjectCreate(Map.of("name","java-legacy"))).get("project"));check(map(map(db.admin().communityProjectGet(Map.of("id",project.get("id")))).get("project")).get("id").equals(project.get("id")));db.admin().communityProjectDelete(Map.of("id",project.get("id")));
                Map<String,Object> key=map(db.security().createApiKey(Map.of("name","java-reader","role","read")));ConnectionOptions parsed=ConnectionOptions.fromUrl(url);String publicUrl=(parsed.isTls()?"pacificdbs":"pacificdb")+"://localhost:"+parsed.getPort()+"/sdk_java";
                try(PacificDBClient reader=PacificDB.fromUrl(publicUrl,options)){reader.security().useToken((String)key.get("key"));check(reader.findOne("docs",Map.of("id","0")).get("value").equals(500));refused(()->reader.insert("docs",Map.of("id","denied")));db.security().revokeApiKey(Map.of("id",key.get("id")));refused(()->reader.find("docs"));}
                try(PacificDBClient anonymous=PacificDB.fromUrl(publicUrl,options)){refused(()->anonymous.listDatabases());}
                check(map(db.security().whoami()).get("role").equals("superadmin"));check(db.admin().healthCheck()!=null);check(db.admin().raftStatus()!=null);check(db.admin().opStatus(Map.of("opId",0))!=null);
                String backup=(String)map(db.backups().create(Map.of("description","java-sdk-qualified"))).get("backup_id");check(db.backups().verify(Map.of("backup_id",backup))!=null);
                Path export=root.resolve("java-backup.json");db.backups().export(backup,export,65536);Map<String,Object> document=JSON.readValue(export.toFile(),Map.class);
                for(Object item:(List<?>)document.get("files")){Map<String,Object> file=map(item);var out=new java.io.ByteArrayOutputStream();for(Object chunk:(List<?>)file.get("chunks"))out.write(Base64.getDecoder().decode((String)chunk));byte[] data=out.toByteArray();check(data.length==((Number)file.get("size_bytes")).longValue()&&sha(data).equals(file.get("sha256")));}
                check(Boolean.TRUE.equals(map(db.backups().restore(Map.of("backup_id",backup,"target_dir",root.resolve("restore/java").toString()))).get("success")));check(Files.isDirectory(root.resolve("restore/java")));
                JSON.writeValue(root.resolve("java-state.json").toFile(),Map.of("expected",expected,"media_id",media.get("id"),"backup_id",backup));
            }
            Map<String,Object> state=JSON.readValue(root.resolve("java-state.json").toFile(),Map.class);
            for(Object item:(List<?>)state.get("expected")){Map<String,Object> row=map(item),actual=db.findOne("docs",Map.of("id",row.get("id")));for(String key:row.keySet())check(Objects.equals(row.get(key),actual.get(key)));}
            for(int i=0;i<32;i++)check(db.findOne("docs",Map.of("id","concurrent-"+i)).get("value").equals(i));
            Path target=root.resolve("java-"+phase+"-download.bin");db.media().downloadFile((String)state.get("media_id"),target,"assets");check(Arrays.equals(source,Files.readAllBytes(target)));check(Arrays.equals(new byte[]{0,(byte)255},(byte[])db.media().get("assets","tiny").get("data")));check(db.backups().verify(Map.of("backup_id",state.get("backup_id")))!=null);
            if("1".equals(System.getenv("PACIFICDB_TEST_TYPED_VECTORS"))){
                Map<String,Object> terms=Map.of("format","sparse-v1","precision","float64",
                    "dimensions",1000000,"indices",List.of(3,999999),"values",List.of(0.125,-2.0));
                if(phase.equals("prepare"))db.vectors().insert(Map.of("collection","vectors","data",
                    Map.of("id","named-typed","vectors",Map.of("dense",List.of(0.123456789012345,1.0),"terms",terms))));
                List<?> hits=(List<?>)map(db.vectors().query(Map.of("collection","vectors","vector",terms,
                    "vector_name","terms","metric","dot","k",10,"search_mode","exact"))).get("data");
                check(hits.size()==1);Map<String,Object> hit=map(hits.get(0));check(hit.get("id").equals("named-typed"));
                check(map(map(hit.get("vectors")).get("terms")).equals(terms));
                check(Math.abs(((Number)hit.get("score")).doubleValue()-4.015625)<1e-12);
            }
            if("1".equals(System.getenv("PACIFICDB_TEST_NATIVE_VECTOR_PREVIEW"))){
                if(phase.equals("prepare")){
                    if("1".equals(System.getenv("PACIFICDB_TEST_VECTOR_SCHEMA")))
                        db.createCollection("preview",Map.of("default",Map.of("dimensions",2,"metric","dot","required",true)));
                    else db.createCollection("preview");
                    db.vectors().insert(Map.of("collection","preview","data",Map.of("id","preview-one","vector",List.of(1,0),"title","rare ocean")));
                    db.vectors().insert(Map.of("collection","preview","data",Map.of("id","preview-two","vector",List.of(0,1),"title","ocean")));
                }
                if("1".equals(System.getenv("PACIFICDB_TEST_VECTOR_SCHEMA")))
                    refused(()->db.insert("preview",Map.of("id","bad-schema","vector",List.of(1))));
                db.vectors().createIndex(Map.of("collection","preview","options",Map.of("name","flat","algorithm","flat",
                    "dimensions",2,"metric","dot","text_field","title","max_elements",100,
                    "max_source_bytes",1048576,"max_index_bytes",4194304,"max_lexical_bytes",1048576)));
                long deadline=System.nanoTime()+30_000_000_000L;
                while(true){
                    Map<String,Object> indexState=map(map(db.vectors().indexStatus(Map.of("collection","preview","name","flat"))).get("index"));
                    if(indexState.get("state").equals("READY"))break;
                    check(!indexState.get("state").equals("FAILED")&&System.nanoTime()<deadline);Thread.sleep(10);
                }
                check(((List<?>)map(db.vectors().listIndexes(Map.of("collection","preview"))).get("indexes")).size()==1);
                Map<String,Object> result=map(db.vectors().query(Map.of("collection","preview","vector",List.of(1,0),"metric","dot",
                    "k",1,"index_name","flat","require_index",true,"search_mode","exact")));
                check(map(((List<?>)result.get("data")).get(0)).get("id").equals("preview-one"));
                check(map(result.get("search")).get("index_type").equals("flat"));
                Map<String,Object> hybrid=map(db.vectors().queryHybrid(Map.of("collection","preview","index_name","flat",
                    "vector",List.of(1,0),"text","rare","metric","dot","k",1,"candidates",2)));
                check(map(((List<?>)hybrid.get("data")).get(0)).get("id").equals("preview-one"));
                db.vectors().rebuildIndex(Map.of("collection","preview","name","flat"));
                if(phase.equals("tls")){
                    db.vectors().dropIndex(Map.of("collection","preview","name","flat"));
                    check(((Number)map(db.count("preview")).get("count")).intValue()==2);
                }
            }
        }
        System.out.println("{\"status\":\"PASS\",\"language\":\"java\",\"phase\":\""+phase+"\",\"exact_documents\":160,\"media_bytes\":700000}");
    }
}
