package com.frostpunk.staticrecomp.probe;
import java.nio.file.*;
import java.util.*;
import java.io.*;

public final class PortProfileCheck {
    private static void require(boolean ok){if(!ok)throw new AssertionError();}
    private static void write(Path path,String text)throws Exception{Files.createDirectories(path.getParent());Files.write(path,text.getBytes("UTF-8"));}
    private static String text(Path path)throws Exception{return new String(Files.readAllBytes(path),"UTF-8");}
    public static void main(String[] args)throws Exception {
        Path work=Files.createTempDirectory("frostpunk-profile-check-");
        Path storage=work.resolve("storage"),old=work.resolve("private/profile");Files.createDirectories(storage);
        write(old.resolve("saves/city.save"),"original city");write(old.resolve("config.bin"),"old settings");
        write(old.getParent().resolve("port-settings.txt"),"ui_scale=170\n");
        Path qa=PortProfile.prepare(storage.toFile(),old.toFile(),"userdata-qa-check").toPath();
        Path invalidSource=work.resolve("not-a-profile");write(invalidSource,"must not inspect");
        require(PortProfile.prepare(storage.toFile(),invalidSource.toFile(),"userdata-qa-check").toPath().equals(qa));
        require(Files.exists(old.resolve("saves/city.save"))&&Files.exists(old.getParent().resolve("port-settings.txt")));
        try(java.util.stream.Stream<Path> paths=Files.list(qa)){require(!paths.findAny().isPresent());}
        Path target=storage.resolve("Frostpunk/userdata");
        write(target.resolve("config.bin"),"new settings");write(target.resolve("saves/keep.save"),"keep me");
        write(target.resolve("port-settings.txt"),"ui_scale=100\n");
        File ready=PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");
        require(ready.toPath().equals(target)&&!Files.exists(old));
        require(text(target.resolve("config.bin")).equals("new settings")&&text(target.resolve("saves/keep.save")).equals("keep me"));
        require(text(target.resolve("saves/city.save")).equals("original city"));
        require(Files.isRegularFile(target.getParent().resolve(".userdata-migration-v1.txt")));
        require(text(target.resolve("port-settings.txt")).equals("ui_scale=170\n")&&!Files.exists(old.getParent().resolve("port-settings.txt")));
        try(java.util.stream.Stream<Path> paths=Files.walk(target.resolve(".migration-backup"))){
            boolean oldSettings=false,oldPreferences=false;
            for(Path backup:(Iterable<Path>)paths.filter(Files::isRegularFile)::iterator){
                String content=text(backup);oldSettings|=content.equals("old settings");oldPreferences|=content.equals("ui_scale=100\n");
            }
            require(oldSettings&&oldPreferences);
        }
        require(PortProfile.prepare(storage.toFile(),old.toFile(),"userdata").toPath().equals(target));
        Path originalReceipt=target.getParent().resolve(".userdata-migration-v1.txt");String originalReceiptText=text(originalReceipt);
        Files.createDirectories(old.resolve("empty-directory"));
        PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");
        require(!Files.exists(old)&&text(originalReceipt).equals(originalReceiptText));
        write(old.getParent().resolve("port-settings.txt"),"ui_scale=160\n");
        PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");
        require(text(target.resolve("port-settings.txt")).equals("ui_scale=160\n")&&!Files.exists(old.getParent().resolve("port-settings.txt")));
        Path receipt=target.getParent().resolve(".userdata-migration-v1.txt");String verifiedReceipt=text(receipt);
        write(receipt,"unrecognized marker owner");write(old.resolve("saves/published-before-completion.save"),"published but keep source");
        try{PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");throw new AssertionError();}catch(IOException expected){}
        require(text(old.resolve("saves/published-before-completion.save")).equals("published but keep source"));
        require(text(target.resolve("saves/published-before-completion.save")).equals("published but keep source"));
        write(receipt,verifiedReceipt);PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");require(!Files.exists(old));
        write(old.resolve("saves/city.save"),"original city");
        PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");require(!Files.exists(old));
        for(String bad:new String[]{"../userdata","userdata/../other","userdata-qa-../evil","/userdata","userdata2","userdata-qa-","userdata\\other"}){
            try{PortProfile.prepare(storage.toFile(),old.toFile(),bad);throw new AssertionError(bad);}catch(IOException expected){}
        }
        write(old.resolve("nested/one.save"),"do not lose");
        write(target.resolve("nested"),"blocking file");
        try{PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");throw new AssertionError();}catch(IOException expected){}
        require(text(old.resolve("nested/one.save")).equals("do not lose"));
        Files.delete(target.resolve("nested"));
        boolean symlink=false;
        try{
            Files.createSymbolicLink(target.resolve("nested"),work);symlink=true;
            try{PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");throw new AssertionError();}catch(IOException expected){}
            require(Files.exists(old.resolve("nested/one.save")));Files.delete(target.resolve("nested"));
        }catch(java.nio.file.FileSystemException unsupported){}
        PortProfile.prepare(storage.toFile(),old.toFile(),"userdata");require(!Files.exists(old));
        try(java.util.stream.Stream<Path> paths=Files.walk(work)){
            for(Path path:(Iterable<Path>)paths.sorted(Comparator.reverseOrder())::iterator)Files.delete(path);
        }
        System.out.println("{\"success\":true,\"copy_missing\":true,\"conflict_backed_up\":true,\"same_hash_rerun\":true,\"qa_never_reads_or_changes_canonical\":true,\"private_preferences_authoritative_then_removed\":true,\"old_cleanup_after_verification\":true,\"failure_retains_source\":true,\"traversal_rejected\":true,\"symlink_case_executed\":"+symlink+"}");
    }
}
