#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/process.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <teoslib/control/config.hpp>
#include <teoslib/control/build_contract.hpp>
#include <teoslib/utilities.hpp>

using namespace std;

namespace teos {
  namespace control {

    static const string contracts_dir = "contracts";
    static const string contract_build_dir = "build";
    static const string template_dir = "templates";    
    static const string skeleton_dir = "skeleton/skeleton.cpp";
    static const string skeleton_name = "skeleton";
    static const string vscode_dir = ".vscode";
    static const string contract_vscode_dir = "contract/.vscode";

    bool process(string command_line, TeosControl* teos_control){
        namespace bp = boost::process;

        bp::ipstream err;
        bp::child c(command_line, bp::std_err > err);

        string err_line;
        string error_msg;
        while (c.running() && std::getline(err, err_line) && !err_line.empty()){
            error_msg += err_line + "\n";
        }
        c.wait();

        if(!error_msg.empty() && error_msg.find("error") != string::npos){
          teos_control->putError(error_msg);
          return false;
        }

        return true;
    }

    vector<string> files(string comma_list, set<string> extensions)
    {
      namespace bfs = boost::filesystem;
            
      vector<string> srcs;
      try{
        bfs::path src_path(wslMapWindowsLinux(comma_list));
        if(bfs::is_directory(src_path)) {
          for (bfs::directory_entry& entry 
            : boost::make_iterator_range(
              bfs::directory_iterator(src_path), {})) 
            {
            if (bfs::exists(entry.path()) 
              && bfs::is_regular_file(entry.path())) 
            {
              bfs::path file = entry.path();
              if(extensions.count(file.extension().string())){
                srcs.push_back(file.string());
              }
            }
          } 
        }
      } catch(...){}

      if(srcs.empty()) 
      {
        vector<string> temp;
        boost::split(temp, comma_list, boost::algorithm::is_any_of(","));

        for(const string& comp: temp){
          bfs::path file(comp);
          if(extensions.count(file.extension().string())){
            srcs.push_back(wslMapWindowsLinux(comp));
          }
        }  
      }
      return srcs;      
    }

    void BootstrapContract::copyTemplate(
      boost::filesystem::path context_path,
      boost::filesystem::path contract_path, 
      string name, string extension)
    {
      namespace bfs = boost::filesystem;

      bfs::path template_path = contract_path / (name + "." + extension);
      if(bfs::exists(template_path)){
        return;
      }      

      respJson_.put("template_" + extension, template_path.string());
      bfs::path skeleton = context_path / template_dir / skeleton_name
        / (skeleton_name + "." + extension);

      if(!bfs::exists(skeleton)){
        return;
      }

      string contents;        
      try{
        bfs::ifstream in(skeleton);
        stringstream ss;
        ss << in.rdbuf();
        in.close();
        contents = ss.str();
        boost::replace_all(contents, skeleton_name, name);
      } catch(exception& e){
        putError(e.what());
      }

      try{
        bfs::ofstream ofs (template_path);
        ofs << contents << endl;
        ofs.flush();
        ofs.close();          
      } catch (bfs::filesystem_error &e){
        putError(e.what());
      }
      if(isError_){
        return;
      }          
    }

    void BootstrapContract::bootstrapContract(string name)
    {
      namespace bfs = boost::filesystem;

      bfs::path context_path(getContextDir(this));
      if(isError_){
        return;
      }
      respJson_.put("context_dir", context_path.string());

      bfs::path contracts_path = context_path / contracts_dir;
      try{
        bfs::create_directory(contracts_path);
      } catch (bfs::filesystem_error &e){
        putError(e.what());
      }
      if(isError_){
        return;
      }
      respJson_.put("contracts_dir", contracts_dir);

      bfs::path contract_path = contracts_path / name;
      try{
        bfs::create_directory(contract_path);
      } catch (bfs::filesystem_error &e){
        putError(e.what());
      }
      if(isError_){
        return;
      }
      respJson_.put("contract_dir", contract_path.string());
      respJson_.put("source_dir", contract_path.string());

      bfs::path build_path = contract_path / contract_build_dir;
      try{
        bfs::create_directory(build_path);
      } catch (bfs::filesystem_error &e){
        putError(e.what());
      }
      if(isError_){
        return;
      }
      respJson_.put("binary_dir", build_path.string());
      copyTemplate(context_path, contract_path, name, "cpp");
      copyTemplate(context_path, contract_path, name, "hpp");

      { /* copy and adapt the template .vscode directory 
      to the contract directory:*/

        bfs::path template_vscode_path = (
          context_path / template_dir / contract_vscode_dir).string();
        bfs::path contract_vscode_path = (contract_path / vscode_dir);
        if(!bfs::exists(contract_vscode_path))
        {         
          try{
            bfs::create_directory(contract_vscode_path);
            for (const auto& dirEnt : bfs::recursive_directory_iterator{template_vscode_path})
            {
                const auto& path = dirEnt.path();
                auto relativePathStr = path.string();
                boost::replace_first(relativePathStr, template_vscode_path.string(), "");
                bfs::copy(path, contract_vscode_path / relativePathStr);
            }
          }catch (exception& e) {
            putError(e.what());
          }

          string contents;
          string tasks_json = "tasks.json";        
          try{
            bfs::ifstream in(contract_vscode_path / tasks_json);
            stringstream ss;
            ss << in.rdbuf();
            contents = ss.str();
            boost::replace_all(contents, "@contract_name@", name);
          } catch(exception& e){
            putError(e.what());
          }

          try{
            bfs::remove(contract_vscode_path / tasks_json);
            bfs::ofstream ofs (contract_vscode_path / tasks_json);
            ofs << contents << endl;
            ofs.flush();
            ofs.close();          
          } catch (bfs::filesystem_error &e){
            putError(e.what());
          }
          if(isError_){
            return;
          } 
        }         
      }
    }

    void GenerateAbi::generateAbi(
      string types_hpp,
      string target_file,
      string include_dir // comma separated list of include dirs
    )
    {
      namespace bfs = boost::filesystem;
      
      vector<string> srcs = files(types_hpp, {".cpp", "c"});
      if(srcs.empty()){
        putError((boost::format("The source is empty. The imput is:\n%1%\n")
              % types_hpp).str());
        return;
      }

      types_hpp = srcs[0];
      
      bfs::path types_pth(types_hpp);
      string name = types_pth.stem().string();
      bfs::path contextFolder = types_pth.parent_path();

      bfs::path target_path(target_file);
      if(target_file.empty()){
        bfs::path build_path(contextFolder / contract_build_dir);
        if(bfs::exists(build_path)){
          target_path = build_path / (name + ".abi");
        } else {
          target_path = contextFolder / (name + ".abi");
        }
      } else {
        target_path = bfs::path(target_file);
        if(!target_path.is_absolute()){
          target_path = contextFolder / target_path;
        }
      }

      string command_line = getSourceDir(this) 
        + "/build/programs/eosio-abigen/eosio-abigen"
        + " -extra-arg=-c -extra-arg=--std=c++14 -extra-arg=--target=wasm32"
        + " -extra-arg=-nostdinc -extra-arg=-nostdinc++ -extra-arg=-DABIGEN"
        + " -extra-arg=-I" + getSourceDir(this) + "/contracts/libc++/upstream/include"
        + " -extra-arg=-I" + getSourceDir(this) + "/contracts/musl/upstream/include"
        + " -extra-arg=-I" + getBOOST_INCLUDE_DIR(this)
        + " -extra-arg=-I" + getSourceDir(this) + "/externals/magic_get/include"
        + " -extra-arg=-I" + getSourceDir(this) + "/contracts"
        + " -extra-arg=-I" + contextFolder.string();

      if(!include_dir.empty())
      {
        vector<string> include_dirs;
        boost::split(include_dirs, include_dir, boost::algorithm::is_any_of(","));
        for (string dir : include_dirs) {
          command_line += " -extra-arg=-I" + dir;
        }
      }

      command_line = command_line
        + " -extra-arg=-fparse-all-comments"
        + " -destination-file=" + target_path.string()
        + " -verbose=0"
        + " -context=" + contextFolder.string()
        + " " + types_pth.string() + " --";
      
      //cout << command_line << endl;

      if(process(command_line, this)){
        boost::property_tree::ptree abi;
        boost::property_tree::read_json(target_path.string(), abi);
        respJson_.add_child("ABI", abi);
        respJson_.put("output", target_path.string());
          //cout << responseToString();        
      }
    };

    void wasmClangHelp()
    {
      string command_line;
      command_line += getWASM_CLANG(nullptr) + " --help";
      boostProcessSystem(command_line);
    }

    /*
    See a basic example of the build procedure: 
      https://gist.github.com/yurydelendik/4eeff8248aeb14ce763e#example.
    */
    void BuildContract::buildContract(
      string src, // comma separated list of source c/cpp files
      string target_file,
      string include_dir // comma separated list of include dirs
    )
    {
      namespace bfs = boost::filesystem;

      bfs::path workdir= bfs::temp_directory_path()
        / bfs::unique_path();
      bfs::create_directories(workdir);
      bfs::path workdir_build(workdir / "build");
      bfs::create_directory(workdir_build);

      vector<string> srcs = files(src, {".cpp", ".c"});
      if(srcs.empty()){
        putError((boost::format("The source is empty. The imput is:\n%1%\n")
              % src).str());
        return;
      }

      string objectFileList;
      bfs::path target_path(target_file);

      for (string file : srcs)
      {  
        bfs::path src_file(file);
        string name = src_file.stem().string();

        if(target_file.empty())
        {
          bfs::path build_path(src_file.parent_path() / contract_build_dir);
          if(bfs::exists(build_path)){
            target_path = build_path / (name + ".wast");
          } else{
            target_path = src_file.parent_path() / (name + ".wast");
          }
        } else 
        {
          target_path = bfs::path(target_file);
          if(!target_path.is_absolute()){
            target_path = src_file.parent_path() / target_path;
          }          
        }

        bfs::path output(workdir_build / (name + ".o"));
        objectFileList += output.string() + " ";

        string command_line;
        command_line += getWASM_CLANG(this)
          + " -emit-llvm -O3 --std=c++14 --target=wasm32 -nostdinc -nostdlib"
          + " -nostdlibinc -ffreestanding -nostdlib -fno-threadsafe-statics"
          + " -fno-rtti -fno-exceptions"
          + " -I" + getSourceDir(this) + "/contracts"
          + " -I" + getSourceDir(this) + "/contracts/libc++/upstream/include"
          + " -I" + getSourceDir(this) + "/contracts/musl/upstream/include"
          + " -I" + getBOOST_INCLUDE_DIR(this)
          + " -I" + getSourceDir(this) + "/externals/magic_get/include"
          + " -I" + src_file.parent_path().string();

        if(!include_dir.empty())
        {
          vector<string> include_dirs;
          boost::split(include_dirs, include_dir, boost::algorithm::is_any_of(","));
          for (string dir : include_dirs) {
            command_line += " -I " + dir;
          }
        }        

        command_line += " -c " + file
          + " -o " + output.string();

        //cout << "command line clang:" << endl << command_line << endl;

        if(!process(command_line, this)){
          return;
        }   
      }

      {
        string command_line;
        command_line += getWASM_LLVM_LINK(this)
          + " -only-needed" 
          + " -o "  + workdir.string() + "/linked.bc"
          + " " + objectFileList // $workdir/built/* DOES NOT WORK
          + " " + getSourceDir(this) + "/build/contracts/musl/libc.bc"
          + " " + getSourceDir(this) + "/build/contracts/libc++/libc++.bc"
          + " " + getSourceDir(this) + "/build/contracts/eosiolib/eosiolib.bc";

        //cout << "command line llvm-link:" << endl << command_line << endl;

        if(!process(command_line, this)){
          return;
        }   
      }
    
      {
        string command_line;
        command_line += getWASM_LLC(this)
          + " -thread-model=single --asm-verbose=false"
          + " -o " + workdir.string() + "/assembly.s"
          + " " + workdir.string() + "/linked.bc";
        //cout << "command line llc:" << endl << command_line << endl;

        if(!process(command_line, this)){
          return;
        } 
      }

      {
        string command_line;
        command_line += getSourceDir(this) + "/build/externals/binaryen/bin/eosio-s2wasm"
          + " -o " + target_path.string()
          + " -s 16384"
          + " " + workdir.string() + "/assembly.s";

        //cout << "command line eosio-s2wasm:" << endl << command_line << endl;

        if(!process(command_line, this)){
          return;
        } 
      }
      bfs::remove_all(workdir);

      ifstream ifs(target_path.string());
      stringstream ss;
      ss << ifs.rdbuf();
      respJson_.put("WAST", ss.str());
      respJson_.put("output", target_path.string());
    }

  }
}