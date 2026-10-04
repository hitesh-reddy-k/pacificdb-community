package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.function.Function;
import static org.junit.jupiter.api.Assertions.*;

class FileTransfersTest {
    @TempDir Path root;
    private static String sha(byte[] bytes) {
        try { return HexFormatForTest.hex(MessageDigest.getInstance("SHA-256").digest(bytes)); }
        catch (Exception error) { throw new AssertionError(error); }
    }
    // Keep test execution compatible with the SDK's Java 11 floor.
    private static class HexFormatForTest {
        static String hex(byte[] data) { StringBuilder out=new StringBuilder(); for(byte b:data) out.append(String.format("%02x", b)); return out.toString(); }
    }
    private static class MediaPeer implements Function<Map<String,Object>,Object> {
        final byte[] source; final Map<Integer,byte[]> chunks=new TreeMap<>(); Map<String,Object> manifest;
        boolean interrupt, badScope; String corrupt; Runnable afterBegin;
        MediaPeer(byte[] source) { this.source=source; }
        Map<String,Object> progress() {
            return Map.of("received_indices", new ArrayList<>(chunks.keySet()), "received_chunks",chunks.size(),
                "received_bytes",chunks.values().stream().mapToInt(b->b.length).sum());
        }
        public Object apply(Map<String,Object> req) {
            switch((String)req.get("action")) {
            case "community_capabilities":return Map.of("max_request_bytes",262144,"media_chunk_source_max_bytes",65536);
            case "community_media_begin":
                if(manifest==null) {
                    manifest=new LinkedHashMap<>();manifest.put("id","media-1");manifest.put("database",req.get("dbName"));
                    for(String key:List.of("collection","filename","content_type","size_bytes","chunk_count","sha256")) manifest.put(key,req.get(key));
                    manifest.put("status","uploading");
                }
                if(afterBegin!=null) afterBegin.run();
                Map<String,Object> m=new LinkedHashMap<>(manifest);m.putAll(progress());if(badScope)m.put("collection","other");return Map.of("media",m);
            case "community_media_put_chunk":
                int index=((Number)req.get("index")).intValue();byte[] data=Base64.getDecoder().decode((String)req.get("data"));
                assertTrue(data.length<=65536);assertEquals(sha(data),req.get("sha256"));assertEquals(data.length,((Number)req.get("size_bytes")).intValue());
                chunks.put(index,data);if(interrupt&&index==1){interrupt=false;return null;}return progress();
            case "community_media_finalize":
                assertArrayEquals(source, chunks.values().stream().collect(java.io.ByteArrayOutputStream::new,
                    (out,b)->out.write(b,0,b.length),(a,b)->{byte[] bs=b.toByteArray();a.write(bs,0,bs.length);}).toByteArray());
                manifest.put("status","ready");return Map.of("media",manifest);
            case "community_media_get":return Map.of("media",manifest);
            case "community_media_get_chunk":
                int i=((Number)req.get("index")).intValue();byte[] b=chunks.get(i);
                Map<String,Object> c=new LinkedHashMap<>(Map.of("media_id","media-1","index",i,"size_bytes",b.length,"sha256",sha(b),"data",Base64.getEncoder().encodeToString(b)));
                if("base64".equals(corrupt))c.put("data","%%%");if("hash".equals(corrupt))c.put("sha256","0".repeat(64));if("index".equals(corrupt))c.put("index",i+1);
                return Map.of("chunk",c);
            default:throw new AssertionError(req);
            }
        }
    }
    private PacificDBClient client(TestPeer peer) { return new PacificDBClient("127.0.0.1",peer.port,"app"); }
    @Test void interruptedResumeDownloadAndScope() throws Exception {
        byte[] source=new byte[150002];new Random(1).nextBytes(source);Path input=root.resolve("sample.bin");Files.write(input,source);
        MediaPeer state=new MediaPeer(source);state.interrupt=true;
        try(TestPeer peer=new TestPeer(state);PacificDBClient db=client(peer)) {
            MediaUploadException fail=assertThrows(MediaUploadException.class,()->db.media().uploadFile("assets",input,null,65536,null));
            assertEquals("media-1",fail.getUploadId());assertEquals(1,fail.getNextChunk());assertEquals(1,fail.getReceivedChunks());assertTrue(fail.isResumable());assertNull(fail.getCause());
            Map<String,Object> ready=db.media().uploadFile("assets",input,null,65536,fail.getUploadId());assertEquals("ready",ready.get("status"));
            assertEquals(List.of(0,1,2),peer.frames.stream().filter(r->r.get("action").equals("community_media_put_chunk")).map(r->r.get("index")).collect(java.util.stream.Collectors.toList()));
            Path dest=root.resolve("out.bin"), other=root.resolve("out.bin.part");Files.writeString(dest,"old");Files.writeString(other,"other transfer");
            for(String corrupt:List.of("base64","hash","index")) {
                state.corrupt=corrupt;assertThrows(PacificDBException.class,()->db.media().downloadFile("media-1",dest,"assets"));
                assertEquals("old",Files.readString(dest));assertEquals("other transfer",Files.readString(other));
            }
            state.corrupt=null;db.media().downloadFile("media-1",dest,"assets");assertArrayEquals(source,Files.readAllBytes(dest));
            assertThrows(PacificDBException.class,()->db.media().downloadFile("media-1",dest,"other"));assertArrayEquals(source,Files.readAllBytes(dest));
            assertTrue(peer.frames.stream().allMatch(r->r.get("dbName").equals("app")));
            assertNoTemporary();
        }
    }
    @Test void scopeMutationDuringHashingAndChunkIoDoesNotRedirect() throws Exception {
        byte[] source=new byte[150000];Path input=root.resolve("file");Files.write(input,source);MediaPeer state=new MediaPeer(source);
        PacificDBClient[] holder=new PacificDBClient[1];
        try(TestPeer peer=new TestPeer(req->{
            if(req.get("action").equals("listDatabases"))return List.of("app","other");
            if(req.get("action").equals("community_capabilities"))holder[0].useDatabase("other");
            return state.apply(req);
        });PacificDBClient db=client(peer)) {
            holder[0]=db;db.media().uploadFile("assets",input,null,65536,null);
            assertEquals("other",db.getDatabase());
            assertTrue(peer.frames.stream().filter(r->!r.get("action").equals("listDatabases")).allMatch(r->r.get("dbName").equals("app")));
        }
    }
    @Test void resumeMismatchAndChangedSourceDoNotFinalize() throws Exception {
        byte[] source=new byte[150000];Path input=root.resolve("file");
        for(String mode:List.of("scope","changed","hash")) {
            Files.write(input,source);MediaPeer state=new MediaPeer(source);
            state.badScope=mode.equals("scope");state.afterBegin=()->{
                try { if(mode.equals("changed"))Files.write(input,new byte[1]);if(mode.equals("hash"))state.manifest.put("sha256","0".repeat(64)); }
                catch(Exception error){throw new AssertionError(error);}
            };
            try(TestPeer peer=new TestPeer(state);PacificDBClient db=client(peer)) {
                assertThrows(PacificDBException.class,()->db.media().uploadFile("assets",input,null,65536,"media-1"));
                assertFalse(peer.frames.stream().anyMatch(r->r.get("action").equals("community_media_finalize")));
            }
        }
    }
    @Test void invalidCapabilitiesAndWireBudgetDoNotBegin() throws Exception {
        Path input=root.resolve("file");Files.write(input,new byte[70000]);
        for(Map<String,Object> caps:List.of(Map.<String,Object>of("max_request_bytes",65536,"media_chunk_source_max_bytes",65536),Map.<String,Object>of("max_request_bytes",262144,"media_chunk_source_max_bytes",0),Map.<String,Object>of("max_request_bytes","bad","media_chunk_source_max_bytes",65536))) {
            try(TestPeer peer=new TestPeer(r->caps);PacificDBClient db=client(peer)) {
                assertThrows(PacificDBException.class,()->db.media().uploadFile("assets",input));assertEquals(1,peer.frames.size());
            }
        }
        try(TestPeer peer=new TestPeer(r->Map.of("max_request_bytes",160000,"media_chunk_source_max_bytes",65536));PacificDBClient db=client(peer)) {
            assertThrows(PacificDBException.class,()->db.media().uploadFile("assets",input,"x".repeat(160000),65536,null));assertEquals(1,peer.frames.size());
        }
    }
    @Test void memoryRoundTripAndMissingFileAreTyped() throws Exception {
        Map<String,Object>[] row=new Map[1];
        try(TestPeer peer=new TestPeer(r->{if(r.get("action").equals("insert")){row[0]=(Map<String,Object>)r.get("data");return Map.of("status","ok");}return Map.of("data",List.of(row[0]));});PacificDBClient db=client(peer)) {
            Map<String,Object> metadata=new HashMap<>(Map.of("contentType","image/png"));byte[] data={0,(byte)255};
            db.media().put("assets","logo",data,metadata);assertArrayEquals(data,(byte[])db.media().get("assets","logo").get("data"));assertEquals(Map.of("contentType","image/png"),metadata);
            assertEquals("file_io_error",assertThrows(PacificDBException.class,()->db.media().uploadFile("assets",root.resolve("missing"))).getCode());
        }
    }
    @Test void streamedBackupValidatesPathsOffsetsHashesAndPreservesDestination() throws Exception {
        byte[] source=new byte[150000];new Random(2).nextBytes(source);Path dest=root.resolve("backup.json");
        for(String mode:List.of("unsafe","duplicate","offset","zero","hash","oversize","good","rename")) {
            if(Files.exists(dest))Files.delete(dest);Files.writeString(dest,"keep");
            try(TestPeer peer=new TestPeer(req->{
                if(req.get("action").equals("export_backup_manifest")) {
                    Map<String,Object> file=Map.of("path",mode.equals("unsafe")?"../file":"data/file","size_bytes",source.length,"sha256",mode.equals("hash")?"0".repeat(64):sha(source));
                    return Map.of("format","pacificdb-full-backup-v1","backup",Map.of("backup_id","backup-1"),"files",mode.equals("duplicate")?List.of(file,file):List.of(file));
                }
                int offset=((Number)req.get("offset")).intValue(),max=((Number)req.get("max_bytes")).intValue();
                byte[] b=Arrays.copyOfRange(source,offset,Math.min(source.length,offset+max));if(mode.equals("zero"))b=new byte[0];if(mode.equals("oversize"))b=Arrays.copyOf(b,b.length+1);
                return Map.of("path",req.get("path"),"offset",mode.equals("offset")?offset+1:offset,"next_offset",offset+b.length,"size_bytes",b.length,"sha256",sha(b),"data",Base64.getEncoder().encodeToString(b));
            });PacificDBClient db=client(peer)) {
                if(mode.equals("good")) {
                    db.backups().export("backup-1",dest,65536);Map<?,?> exported=new ObjectMapper().readValue(dest.toFile(),Map.class);List<?> files=(List<?>)exported.get("files");List<?> chunks=(List<?>)((Map<?,?>)files.get(0)).get("chunks");assertEquals(3,chunks.size());
                    var out=new java.io.ByteArrayOutputStream();for(Object chunk:chunks)out.write(Base64.getDecoder().decode((String)chunk));assertArrayEquals(source,out.toByteArray());
                } else {
                    Path target=dest;if(mode.equals("rename")){target=root.resolve("directory");Files.createDirectory(target);Files.writeString(target.resolve("sentinel"),"keep");}
                    Path finalTarget=target;assertThrows(PacificDBException.class,()->db.backups().export("backup-1",finalTarget,65536));assertEquals("keep",Files.readString(dest));
                    if(mode.equals("rename"))assertEquals("keep",Files.readString(target.resolve("sentinel")));
                }
                assertNoTemporary();
            }
        }
    }
    private void assertNoTemporary() throws Exception {try(var paths=Files.list(root)){assertFalse(paths.anyMatch(p->p.getFileName().toString().startsWith(".pacificdb-")));}}
}
