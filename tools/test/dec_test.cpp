#include "Helper/DecompileHelper.h"
#include "Helper/DebugHelper.h"

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

#include <iostream>
#include <vector>
#include <map>
#include <string>

using namespace boost;
using namespace std;
using namespace llvm;

void handle_quit(const vector<string>& args);
void handle_launch(const vector<string>& args);
void handle_dec_by_name(const vector<string>& args);
void handle_dec_by_addr(const vector<string>& args);

map<string, void(*)(const vector<string>&)> cmd_func_map = {
  {"q", handle_quit},
  {"launch", handle_launch},
  {"dec_by_name", handle_dec_by_name},
  {"dec_by_addr", handle_dec_by_addr},
};

void print_prompt()
{
  cout << "(dec test) ";
}

vector<string> split_str(string str)
{
  vector<string> strs;
  split(strs, str, is_any_of(" "), token_compress_on);
  return strs;
}

void handle_cmd(vector<string> strs)
{
  assert(cmd_func_map.find(strs[0])!=cmd_func_map.end());
  cmd_func_map[strs[0]](vector<string>(++strs.begin(), strs.end()));
}

int main(int argc, char** argv)
{
  if(argc!=1)
  {
    create_debugger(argv[1]);
    ::uint64_t addr = stoull(argv[2], 0, 0);
    assert(addr);
    get_module_with_function(addr);
    cout << "dec done" << endl;
    return 0;
  }
  while(1)
  {
    print_prompt();
    string input;
    getline(cin, input);
    vector<string> strs = split_str(input);
    handle_cmd(strs);
  }
  
  return 0;
}

void handle_quit(const vector<string>& args)
{
  assert(args.empty());
  cout << "quit now" << endl;
  exit(0);
}

void handle_launch(const vector<string>& args)
{
  assert(args.size()==1);
  Module* init_mdl = get_module(args[0]);
  cout << "luanch done: " << init_mdl->getModuleIdentifier() << endl;
}

void handle_dec_by_name(const vector<string>& args)
{
  assert(args.size()==1);
  get_module_with_function(args[0]);
  cout << "dec done" << endl;
}

void handle_dec_by_addr(const vector<string>& args)
{
  assert(args.size()==1);
  ::uint64_t addr = stoull(args[0], 0, 0);
  get_module_with_function(addr);
  cout << "dec done" << endl;
}