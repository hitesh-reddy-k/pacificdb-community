package io.pacificdb;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.*;
import java.nio.channels.Channels;
import java.nio.channels.SeekableByteChannel;
import java.nio.file.*;
import java.nio.file.attribute.*;
import java.security.*;
import java.util.*;

/** Verified bounded transfers. All requests retain the scope captured at entry. */
final class FileTransfers {
    private static final int MAX_CHUNK=4*1024*1024, MIN_CHUNK=65536, RESERVE=65536;
    private static final long MAX_INTEGER=9007199254740991L;
    private static final ObjectMapper JSON=new ObjectMapper();
    private FileTransfers() { }
    private static PacificDBException fail() { return new PacificDBException("invalid_transfer_response"); }
    private static String name(String value) { if(value==null||value.isEmpty())throw new PacificDBException("invalid_transfer_argument");return value; }
    @SuppressWarnings("unchecked") private static Map<String,Object> object(Object value) { if(!(value instanceof Map))throw fail();return (Map<String,Object>)value; }
    private static long integer(Object value,long min,long max) {
        if(!(value instanceof Byte||value instanceof Short||value instanceof Integer||value instanceof Long))throw fail();
        long n=((Number)value).longValue();if(n<min||n>max)throw fail();return n;
    }
    private static long integer(Object value) {return integer(value,0,MAX_INTEGER);}
    private static String hash(Object value) {if(!(value instanceof String)||!((String)value).matches("[a-fA-F0-9]{64}"))throw fail();return ((String)value).toLowerCase(Locale.ROOT);}
    private static MessageDigest digest() {try{return MessageDigest.getInstance("SHA-256");}catch(NoSuchAlgorithmException e){throw new AssertionError(e);}}
    private static String hex(byte[] data) {StringBuilder out=new StringBuilder(64);for(byte b:data){out.append(Character.forDigit((b>>4)&15,16));out.append(Character.forDigit(b&15,16));}return out.toString();}
    private static String sha(byte[] data) {return hex(digest().digest(data));}
    private static byte[] decode(Object value,long maximum) {
        if(!(value instanceof String)||((String)value).length()>4*((maximum+2)/3))throw fail();
        byte[] result;try{result=Base64.getDecoder().decode((String)value);}catch(IllegalArgumentException e){throw fail();}
        if(result.length>maximum||!Base64.getEncoder().encodeToString(result).equals(value))throw fail();return result;
    }
    private static PacificDBClient.ScopedRequester capture(PacificDBClient db,String collection) {
        synchronized(db){if(db.getDatabase().isEmpty())throw new PacificDBException("database_required");if(collection!=null)name(collection);return db.scopedRequester();}
    }
    private static Map<String,Object> manifest(Object value,String database,String collection,String id) {
        Map<String,Object> m=object(value);name(m.get("id") instanceof String?(String)m.get("id"):null);
        if(!database.equals(m.get("database"))||!(m.get("collection") instanceof String)||((String)m.get("collection")).isEmpty()
            ||collection!=null&&!collection.equals(m.get("collection"))||id!=null&&!id.equals(m.get("id")))throw new PacificDBException("media_scope_mismatch");
        long size=integer(m.get("size_bytes"),1,MAX_INTEGER);integer(m.get("chunk_count"),1,size);hash(m.get("sha256"));return m;
    }
    static Object put(PacificDBClient db,String collection,String id,byte[] bytes,Map<String,Object> metadata) {
        PacificDBClient.ScopedRequester request=capture(db,name(collection));name(id);
        if(bytes==null||metadata==null)throw new PacificDBException("invalid_media_data");byte[] owned=bytes.clone();
        Map<String,Object> row=new LinkedHashMap<>(metadata);row.put("id",id);row.put("kind","media");row.put("encoding","base64");row.put("sizeBytes",owned.length);row.put("dataBase64",Base64.getEncoder().encodeToString(owned));
        return request.apply(Map.of("action","insert","collection",collection,"data",row));
    }
    static Map<String,Object> get(PacificDBClient db,String collection,String id) {
        name(id);Map<String,Object> row=db.findOne(collection,Map.of("id",id));
        if(row==null||!id.equals(row.get("id"))||!"media".equals(row.get("kind"))||!"base64".equals(row.get("encoding")))throw new PacificDBException("media_not_found");
        long size=integer(row.get("sizeBytes"),0,64*1024*1024);byte[] data=decode(row.get("dataBase64"),size);if(data.length!=size)throw fail();
        Map<String,Object> metadata=new LinkedHashMap<>(row);metadata.remove("dataBase64");return Map.of("data",data,"metadata",metadata);
    }
    private static Object send(PacificDBClient.ScopedRequester request,Map<String,Object> command,long limit) {
        if(request.wireBytes(command)>limit)throw new PacificDBException("media_request_too_large");return request.apply(command);
    }
    private static Set<Long> progress(Object value,Set<Long> previous,Long added,long count,long size,int chunk) {
        Map<String,Object> m=object(value);Object indices=m.get("received_indices");
        if(!(indices instanceof List)||((List<?>)indices).size()>count)throw fail();Set<Long> current=new HashSet<>();long total=0;
        for(Object index:(List<?>)indices){long n=integer(index,0,count-1);if(!current.add(n))throw fail();total+=Math.min(chunk,size-n*chunk);}
        if(!current.containsAll(previous)||added!=null&&!current.contains(added)||integer(m.get("received_bytes"))!=total||integer(m.get("received_chunks"))!=current.size())throw fail();return current;
    }
    static Map<String,Object> upload(PacificDBClient db,String collection,Path path,String contentType,Integer requested,String resume) {
        PacificDBClient.ScopedRequester request=capture(db,name(collection));Objects.requireNonNull(path);if(resume!=null)name(resume);
        if(requested!=null)integer(requested,MIN_CHUNK,MAX_CHUNK);
        if(!Files.isRegularFile(path))throw new PacificDBException("file_io_error");
        try {
            // One open file description is used for hashing and sending; a pathname replacement cannot redirect reads.
            try(SeekableByteChannel channel=Files.newByteChannel(path,StandardOpenOption.READ);InputStream input=Channels.newInputStream(channel)) {
                if(!Files.isRegularFile(path))throw new PacificDBException("media_file_not_regular");
                long size=integer(channel.size());if(size==0)throw new PacificDBException("media_file_empty");
                FileTime modified=Files.getLastModifiedTime(path);
                String type=contentType==null?Files.probeContentType(path):contentType;if(type==null)type="application/octet-stream";name(type);
                Map<String,Object> caps=object(request.apply(Map.of("action","community_capabilities")));
                long limit=integer(caps.get("max_request_bytes"),RESERVE+1,MAX_INTEGER),engine=integer(caps.get("media_chunk_source_max_bytes"),1,MAX_INTEGER);
                int chunk=(int)Math.min(Math.min(MAX_CHUNK,engine),Math.min((limit-RESERVE)*3/4,requested==null?MAX_CHUNK:requested));
                if(chunk<MIN_CHUNK)throw new PacificDBException("media_chunk_limit_too_small");long count=(size+chunk-1)/chunk;
                MessageDigest whole=digest();byte[] buffer=new byte[chunk];int read;
                while((read=input.read(buffer))!=-1)whole.update(buffer,0,read);String expected=hex(whole.digest());channel.position(0);
                Map<String,Object> command=new LinkedHashMap<>(Map.of("action","community_media_begin","collection",collection,"filename",path.getFileName().toString(),"content_type",type,"size_bytes",size,"chunk_count",count,"sha256",expected));if(resume!=null)command.put("resume_id",resume);
                Map<String,Object> begun=object(send(request,command,limit));Map<String,Object> m=manifest(begun.get("media"),request.database,collection,resume);
                if(!path.getFileName().toString().equals(m.get("filename"))||!type.equals(m.get("content_type"))||integer(m.get("size_bytes"))!=size||!expected.equals(hash(m.get("sha256"))))throw new PacificDBException("media_resume_mismatch");
                if("ready".equals(m.get("status")))return m;
                if(!"uploading".equals(m.get("status"))||integer(m.get("chunk_count"))!=count)throw new PacificDBException("media_resume_mismatch");
                Set<Long> received=progress(m,Set.of(),null,count,size,chunk);String id=(String)m.get("id");whole=digest();
                try {
                    for(long index=0;index<count;index++) {
                        int length=(int)Math.min(chunk,size-index*chunk);byte[] data=input.readNBytes(length);if(data.length!=length)throw new PacificDBException("media_file_changed");whole.update(data);
                        if(received.contains(index))continue;
                        Map<String,Object> stored=object(send(request,Map.of("action","community_media_put_chunk","media_id",id,"index",index,"data",Base64.getEncoder().encodeToString(data),"size_bytes",length,"sha256",sha(data)),limit));
                        received=progress(stored.containsKey("media")?stored.get("media"):stored,received,index,count,size,chunk);
                    }
                    if(!expected.equals(hex(whole.digest()))||input.read()!=-1||channel.size()!=size||!Files.getLastModifiedTime(path).equals(modified))throw new PacificDBException("media_file_changed");
                    Map<String,Object> finalized=object(send(request,Map.of("action","community_media_finalize","media_id",id),limit));Map<String,Object> ready=manifest(finalized.get("media"),request.database,collection,id);
                    if(!"ready".equals(ready.get("status"))||integer(ready.get("size_bytes"))!=size||!hash(ready.get("sha256")).equals(expected)||integer(ready.get("chunk_count"))!=count)throw fail();return ready;
                } catch(IOException|PacificDBException error) {
                    long next=0,total=0;while(next<count&&received.contains(next))next++;for(long i:received)total+=Math.min(chunk,size-i*chunk);
                    String code=error instanceof PacificDBException?((PacificDBException)error).getCode():"file_io_error";
                    throw new MediaUploadException("media_upload_interrupted","Media upload interrupted; resume explicitly",error instanceof PacificDBException?((PacificDBException)error).getResponse():null,id,next,received.size(),total,
                        !Set.of("media_file_changed","media_chunk_conflict","media_upload_expired","media_upload_not_resumable").contains(code));
                }
            }
        } catch(IOException error){throw new PacificDBException("file_io_error");}
    }
    private interface Writer {void write(OutputStream out) throws IOException;}
    private static void destination(Path path,Writer writer) throws IOException {
        Path target=path.toAbsolutePath();Path parent=target.getParent();Path temporary;
        if(Files.getFileStore(parent).supportsFileAttributeView(PosixFileAttributeView.class))
            temporary=Files.createTempFile(parent,".pacificdb-",".tmp",PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rw-------")));
        else temporary=Files.createTempFile(parent,".pacificdb-",".tmp");
        try {
            try(OutputStream out=Files.newOutputStream(temporary)){writer.write(out);}
            // No unsafe copy fallback if atomic replacement is unavailable.
            Files.move(temporary,target,StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);
        } finally {Files.deleteIfExists(temporary);}
    }
    static Map<String,Object> download(PacificDBClient db,String id,Path destination,String collection) {
        PacificDBClient.ScopedRequester request=capture(db,collection);name(id);
        Map<String,Object> m=manifest(object(request.apply(Map.of("action","community_media_get","media_id",id))).get("media"),request.database,collection,id);
        if(!"ready".equals(m.get("status")))throw new PacificDBException("media_not_ready");
        long[] total={0};MessageDigest whole=digest();String expected=hash(m.get("sha256"));
        try {
            destination(destination,out->{
                long size=integer(m.get("size_bytes")),count=integer(m.get("chunk_count"));
                for(long index=0;index<count;index++) {
                    Map<String,Object> c=object(object(request.apply(Map.of("action","community_media_get_chunk","media_id",id,"index",index))).get("chunk"));
                    if(!id.equals(c.get("media_id"))||integer(c.get("index"))!=index)throw fail();byte[] data=decode(c.get("data"),Math.min(MAX_CHUNK,size-total[0]));
                    if(data.length==0||integer(c.get("size_bytes"))!=data.length||!sha(data).equals(hash(c.get("sha256"))))throw fail();
                    out.write(data);whole.update(data);total[0]+=data.length;
                }
                if(total[0]!=size||!hex(whole.digest()).equals(expected))throw new PacificDBException("media_checksum_mismatch");
            });return Map.of("id",id,"destination",destination.toString(),"size_bytes",total[0],"sha256",expected);
        }catch(IOException error){throw new PacificDBException("file_io_error");}
    }
    static Map<String,Object> export(PacificDBClient db,String id,Path destination,int chunkBytes) {
        PacificDBClient.ScopedRequester request=db.scopedRequester();name(id);integer(chunkBytes,1,1048576);
        Map<String,Object> m=object(request.apply(Map.of("action","export_backup_manifest","backup_id",id)));
        if(!"pacificdb-full-backup-v1".equals(m.get("format"))||!id.equals(object(m.get("backup")).get("backup_id"))||!(m.get("files") instanceof List))throw fail();
        Set<String> paths=new HashSet<>();List<?> files=(List<?>)m.get("files");
        for(Object item:files) {
            Map<String,Object> file=object(item);Object raw=file.get("path");if(!(raw instanceof String))throw fail();String path=(String)raw;
            if(path.isEmpty()||path.length()>4096||path.indexOf('\\')>=0||path.indexOf(':')>=0||path.chars().anyMatch(c->c<32||c==127)||Arrays.stream(path.split("/",-1)).anyMatch(p->p.isEmpty()||p.equals(".")||p.equals(".."))||!paths.add(path))throw new PacificDBException("invalid_backup_path");
            integer(file.get("size_bytes"));hash(file.get("sha256"));
        }
        long[] total={0};
        try {
            destination(destination,out->{
                write(out,"{\"format\":"+JSON.writeValueAsString(m.get("format"))+",\"backup\":"+JSON.writeValueAsString(m.get("backup"))+",\"files\":[");int number=0;
                for(Object item:files) {
                    Map<String,Object> file=object(item);String path=(String)file.get("path");long size=integer(file.get("size_bytes"));
                    write(out,(number++>0?",":"")+"{\"path\":"+JSON.writeValueAsString(path)+",\"size_bytes\":"+size+",\"sha256\":"+JSON.writeValueAsString(file.get("sha256"))+",\"chunks\":[");
                    long offset=0;MessageDigest checksum=digest();int chunkIndex=0;
                    while(offset<size) {
                        int maximum=(int)Math.min(chunkBytes,size-offset);Map<String,Object> c=object(request.apply(Map.of("action","export_backup_file_chunk","backup_id",id,"path",path,"offset",offset,"max_bytes",maximum)));byte[] data=decode(c.get("data"),maximum);
                        if(data.length==0||!path.equals(c.get("path"))||integer(c.get("offset"))!=offset||integer(c.get("next_offset"))!=offset+data.length||integer(c.get("size_bytes"))!=data.length||!sha(data).equals(hash(c.get("sha256"))))throw fail();
                        write(out,(chunkIndex++>0?",":"")+JSON.writeValueAsString(c.get("data")));checksum.update(data);offset+=data.length;total[0]+=data.length;
                    }
                    if(!hex(checksum.digest()).equals(hash(file.get("sha256"))))throw new PacificDBException("backup_checksum_mismatch");write(out,"]}");
                }
                write(out,"]}\n");
            });return Map.of("backup_id",id,"destination",destination.toString(),"files",files.size(),"size_bytes",total[0]);
        }catch(IOException error){throw new PacificDBException("file_io_error");}
    }
    private static void write(OutputStream out,String value) throws IOException {out.write(value.getBytes(java.nio.charset.StandardCharsets.UTF_8));}
}
