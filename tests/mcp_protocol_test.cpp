#include "mcp/McpProtocol.hpp"
#include <iostream>
#include <stdexcept>
using namespace vlt::mcp;
void require(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
int main() try {
    Protocol legacy;
    const auto dispatch=[](const std::string &name,const Json &) { return name=="tools/list"?Json{{"tools",Json::array()}}:toolResult(true,{{"value",42}}); };
    auto request=[](int id,const char *method,Json params=Json::object()) { return Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}; };
    require(legacy.receive(request(1,"tools/list"),dispatch)->contains("error"),"legacy initialization enforced");
    auto result=legacy.receive(request(2,"initialize",{{"protocolVersion",legacyVersion},{"capabilities",Json::object()},{"clientInfo",{{"name","test"},{"version","1"}}}}),dispatch);
    require((*result)["result"]["protocolVersion"]==legacyVersion,"legacy negotiation");
    require(!legacy.receive({{"jsonrpc","2.0"},{"method","notifications/initialized"}},dispatch),"notification response");
    result=legacy.receive(request(3,"tools/list"),dispatch);
    require((*result)["result"]["tools"].is_array(),"legacy tools");
    require(!(*result)["result"].contains("resultType"),"legacy stays legacy");
    Protocol modern;
    Json meta={{"io.modelcontextprotocol/protocolVersion",modernVersion},{"io.modelcontextprotocol/clientCapabilities",Json::object()}};
    result=modern.receive(request(4,"server/discover",{{"_meta",meta}}),dispatch);
    require((*result)["result"]["resultType"]=="complete","modern discovery");
    result=modern.receive(request(5,"tools/call",{{"_meta",meta},{"name","get_project"}}),dispatch);
    require((*result)["result"]["structuredContent"]["value"]==42,"stateless modern call");
    meta["io.modelcontextprotocol/protocolVersion"]="future";
    result=modern.receive(request(6,"tools/list",{{"_meta",meta}}),dispatch);
    require((*result)["error"]["code"]==-32022,"unsupported modern version");
    for(auto malformed:{Json::array(),Json(2),Json{{"jsonrpc","2.0"},{"id",nullptr},{"method","ping"}},Json{{"jsonrpc","2.0"},{"id",3},{"method",4}}})
        require(legacy.receive(malformed,dispatch)->at("error").at("code")==-32600,"invalid request");
    bool cancelled=false;
    legacy.receive({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",{{"requestId",7}}}},dispatch,[&](const Json &id){cancelled=id==7;});
    require(cancelled,"cancellation");
    bool rejected=false;
    try {validate({{"type","object"},{"properties",{{"n",{{"type","integer"},{"minimum",0}}}}},{"required",{"n"}},{"additionalProperties",false}},{{"n",-1}});} catch(...) {rejected=true;}
    require(rejected,"schema bounds");
    std::cout<<"MCP protocol checks passed\n"; return 0;
} catch(const std::exception &e) {std::cerr<<e.what()<<'\n'; return 1;}
