package com.frostpunk.staticrecomp.probe;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.Map;
import java.util.TreeMap;

final class PortProfile {
    private static final LinkOption[] NO_LINKS={LinkOption.NOFOLLOW_LINKS};

    static String leaf(String value) throws IOException {
        if(value==null||value.isEmpty())return "userdata";
        if(!value.matches("userdata(?:-qa-[A-Za-z0-9_-]{1,48})?"))throw new IOException("Invalid userdata folder name");
        return value;
    }
    private static void directory(Path path) throws IOException {
        if(Files.exists(path,NO_LINKS)) {
            if(Files.isSymbolicLink(path)||!Files.isDirectory(path,NO_LINKS))throw new IOException("Profile directory is not a real directory");
        } else Files.createDirectory(path);
    }
    private static String hash(Path path) throws IOException {
        if(!Files.isRegularFile(path,NO_LINKS)||Files.isSymbolicLink(path))throw new IOException("Profile entry is not a regular file");
        try {
            MessageDigest digest=MessageDigest.getInstance("SHA-256");
            try(InputStream input=Files.newInputStream(path,LinkOption.NOFOLLOW_LINKS)) {
                byte[] buffer=new byte[65536];int count;
                while((count=input.read(buffer))!=-1)digest.update(buffer,0,count);
            }
            StringBuilder result=new StringBuilder(64);
            for(byte value:digest.digest())result.append(String.format(java.util.Locale.ROOT,"%02x",value&255));
            return result.toString();
        } catch(java.security.NoSuchAlgorithmException error){throw new IOException(error);}
    }
    private static void inventory(Path root,Path current,Map<String,String> files) throws IOException {
        if(Files.isSymbolicLink(current)||!Files.isDirectory(current,NO_LINKS))throw new IOException("Unsafe old profile directory");
        try(java.nio.file.DirectoryStream<Path> children=Files.newDirectoryStream(current)) {
            for(Path child:children) {
                BasicFileAttributes info=Files.readAttributes(child,BasicFileAttributes.class,LinkOption.NOFOLLOW_LINKS);
                if(info.isDirectory())inventory(root,child,files);
                else if(info.isRegularFile()&&!info.isSymbolicLink())files.put(root.relativize(child).toString(),hash(child));
                else throw new IOException("Unsupported old profile entry");
            }
        }
    }
    private static void parents(Path root,Path destination) throws IOException {
        Path current=root;
        for(Path part:root.relativize(destination.getParent())){current=current.resolve(part);directory(current);}
    }
    private static void publish(Path root,Path source,Path target,String digest) throws IOException {
        publish(root,source,target,digest,false);
    }
    private static void publish(Path root,Path source,Path target,String digest,boolean replace) throws IOException {
        parents(root,target);
        String original=null;
        if(Files.exists(target,NO_LINKS)) {
            original=hash(target);if(original.equals(digest))return;
            if(!replace)throw new IOException("Migration backup conflicts with source");
        }
        Path temporary=Files.createTempFile(target.getParent(),".profile-migration-",".tmp");
        try {
            try(InputStream input=Files.newInputStream(source,LinkOption.NOFOLLOW_LINKS);
                OutputStream output=Files.newOutputStream(temporary,StandardOpenOption.WRITE)) {
                byte[] buffer=new byte[65536];int count;
                while((count=input.read(buffer))!=-1)output.write(buffer,0,count);
            }
            if(!hash(temporary).equals(digest)||!hash(source).equals(digest))throw new IOException("Profile changed during migration");
            try(java.nio.channels.FileChannel file=java.nio.channels.FileChannel.open(temporary,StandardOpenOption.WRITE)){file.force(true);}
            if(replace) {
                if(original==null||!hash(target).equals(original))throw new IOException("Destination changed during migration");
                Files.move(temporary,target,java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            } else Files.move(temporary,target);
            if(!hash(target).equals(digest))throw new IOException("Published profile checksum mismatch");
        } finally {Files.deleteIfExists(temporary);}
    }
    private static void removeEmptyTree(Path current) throws IOException {
        if(Files.isSymbolicLink(current))throw new IOException("Profile became a symbolic link");
        try(java.nio.file.DirectoryStream<Path> children=Files.newDirectoryStream(current)) {
            for(Path child:children) {
                if(!Files.isDirectory(child,NO_LINKS))throw new IOException("Profile changed before cleanup");
                removeEmptyTree(child);
            }
        }
        Files.delete(current);
    }
    private static void preferences(Path target,Path source) throws IOException {
        if(!Files.exists(source,NO_LINKS))return;
        String digest=hash(source);Path destination=target.resolve("port-settings.txt");
        boolean exists=Files.exists(destination,NO_LINKS);String previous=exists?hash(destination):null;
        if(!digest.equals(previous)) {
            long needed=Files.size(source);
            Path backup=previous==null?null:target.resolve(".migration-backup").resolve(previous).resolve("port-settings.txt");
            if(backup!=null&&!Files.exists(backup,NO_LINKS))needed=Math.addExact(needed,Files.size(destination));
            if(target.toFile().getUsableSpace()<needed)throw new IOException("Not enough space to migrate preferences");
            if(backup!=null)publish(target,destination,backup,previous);
            publish(target,source,destination,digest,exists);
        }
        if(!hash(source).equals(digest)||!hash(destination).equals(digest))throw new IOException("Preferences changed before cleanup");
        Files.delete(source);
    }
    static synchronized File prepare(File storage,File oldProfile,String requestedLeaf) throws IOException {
        String name=leaf(requestedLeaf);
        Path base=storage.getCanonicalFile().toPath();
        if(Files.isSymbolicLink(storage.toPath()))throw new IOException("Storage root is a symbolic link");
        Path root=base.resolve("Frostpunk");directory(root);
        Path target=root.resolve(name);directory(target);
        if(!target.toFile().getCanonicalFile().toPath().equals(target))throw new IOException("Profile escaped its storage root");
        if(!name.equals("userdata"))return target.toFile();
        Path old=oldProfile.toPath();
        Path privatePreferences=oldProfile.toPath().getParent().resolve("port-settings.txt");
        if(!Files.exists(old,NO_LINKS)){preferences(target,privatePreferences);return target.toFile();}
        Path original=oldProfile.getCanonicalFile().toPath();
        if(original.startsWith(root)||root.startsWith(original))throw new IOException("Old and persistent profiles overlap");
        TreeMap<String,String> before=new TreeMap<>();inventory(old,old,before);
        if(before.isEmpty()){removeEmptyTree(old);preferences(target,privatePreferences);return target.toFile();}
        long needed=0;
        for(Map.Entry<String,String> entry:before.entrySet()) {
            Path destination=target.resolve(entry.getKey());parents(target,destination);
            if(Files.exists(destination,NO_LINKS)) {
                if(hash(destination).equals(entry.getValue()))continue;
                Path backup=target.resolve(".migration-backup").resolve(entry.getValue()).resolve(entry.getKey());
                if(Files.exists(backup,NO_LINKS)&&hash(backup).equals(entry.getValue()))continue;
            }
            needed=Math.addExact(needed,Files.size(old.resolve(entry.getKey())));
        }
        if(target.toFile().getUsableSpace()<needed)throw new IOException("Not enough space to migrate saved games");
        TreeMap<String,String> destinations=new TreeMap<>();
        for(Map.Entry<String,String> entry:before.entrySet()) {
            Path source=old.resolve(entry.getKey()),destination=target.resolve(entry.getKey());
            parents(target,destination);
            if(Files.exists(destination,NO_LINKS)&&!hash(destination).equals(entry.getValue()))
                destination=target.resolve(".migration-backup").resolve(entry.getValue()).resolve(entry.getKey());
            publish(target,source,destination,entry.getValue());
            destinations.put(entry.getKey(),target.relativize(destination).toString());
        }
        TreeMap<String,String> after=new TreeMap<>();inventory(old,old,after);
        if(!before.equals(after))throw new IOException("Old profile changed during migration");
        for(Map.Entry<String,String> entry:before.entrySet())
            if(!hash(target.resolve(destinations.get(entry.getKey()))).equals(entry.getValue()))throw new IOException("Migrated profile changed before completion");
        Path receipt=root.resolve("."+name+"-migration-v1.txt");
        if(Files.isSymbolicLink(receipt))throw new IOException("Unsafe migration receipt");
        String signature="Frostpunk private-profile migration v1\n";
        if(Files.exists(receipt,NO_LINKS)&&!new String(Files.readAllBytes(receipt),java.nio.charset.StandardCharsets.UTF_8).startsWith(signature))
            throw new IOException("Unrecognized migration receipt already exists");
        Path temporary=Files.createTempFile(root,".profile-migration-receipt-",".tmp");
        try {
            StringBuilder text=new StringBuilder(signature+"sha256\tsource\tdestination\n");
            for(Map.Entry<String,String> entry:before.entrySet())text.append(entry.getValue()).append('\t').append(entry.getKey()).append('\t').append(destinations.get(entry.getKey())).append('\n');
            Files.write(temporary,text.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8),StandardOpenOption.TRUNCATE_EXISTING);
            try(java.nio.channels.FileChannel file=java.nio.channels.FileChannel.open(temporary,StandardOpenOption.WRITE)){file.force(true);}
            Files.move(temporary,receipt,java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } finally {Files.deleteIfExists(temporary);}
        for(Map.Entry<String,String> entry:before.entrySet()) {
            Path source=old.resolve(entry.getKey());
            if(!hash(source).equals(entry.getValue())||!hash(target.resolve(destinations.get(entry.getKey()))).equals(entry.getValue()))
                throw new IOException("Profile changed before old-copy cleanup");
            Files.delete(source);
        }
        removeEmptyTree(old);preferences(target,privatePreferences);return target.toFile();
    }
}
