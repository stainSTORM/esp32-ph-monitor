#ifndef AGENT_H
#define AGENT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <functional>
#include <map>
#include "app.h"
#include "reply_channel.h"

// Forward declarations
class Agent;
class AgentState;
class ArkitektApp;

// Function callback type: receives (ArkitektApp&, Agent&, JsonObject args, ReplyChannel&)
typedef std::function<bool(ArkitektApp &, Agent &, JsonObject, ReplyChannel &)> AgentFunction;

// Definition structure for a function
struct FunctionDefinition
{
    String key;
    String name;
    String description;
    String version;
    String kind; // "FUNCTION" or "GENERATOR"
    bool stateful;
    bool isDev;
    JsonArray collections;
    JsonArray interfaces;
    JsonArray isTestFor;
    JsonArray args;       // Array of PortInput definitions
    JsonArray returns;    // Array of PortInput definitions
    JsonArray portGroups; // Array of PortGroupInput definitions

    // Owned backing storage (managed by builder, transferred on build())
    DynamicJsonDocument* _argsDoc = nullptr;
    DynamicJsonDocument* _returnsDoc = nullptr;

    FunctionDefinition() : stateful(false), isDev(false), kind("FUNCTION"), version("0.0.1") {}

    FunctionDefinition(const String &name, const String &desc)
        : key(name), name(name), description(desc), stateful(false), isDev(false), kind("FUNCTION"), version("0.0.1") {}

    void toJson(JsonObject obj) const
    {
        obj["key"] = key;
        obj["name"] = name;
        obj["version"] = version;
        obj["description"] = description;
        obj["kind"] = kind;
        obj["stateful"] = stateful;
        obj["isDev"] = isDev;

        if (collections.size() > 0)
        {
            obj["collections"] = collections;
        }
        else
        {
            JsonArray emptyCollections = obj["collections"].to<JsonArray>();
        }

        if (interfaces.size() > 0)
        {
            obj["interfaces"] = interfaces;
        }
        else
        {
            JsonArray emptyInterfaces = obj["interfaces"].to<JsonArray>();
        }

        if (isTestFor.size() > 0)
        {
            obj["isTestFor"] = isTestFor;
        }
        else
        {
            JsonArray emptyIsTestFor = obj["isTestFor"].to<JsonArray>();
        }

        if (args.size() > 0)
        {
            obj["args"] = args;
        }
        else
        {
            JsonArray emptyArgs = obj["args"].to<JsonArray>();
        }

        if (returns.size() > 0)
        {
            obj["returns"] = returns;
        }
        else
        {
            JsonArray emptyReturns = obj["returns"].to<JsonArray>();
        }

        if (portGroups.size() > 0)
        {
            obj["portGroups"] = portGroups;
        }
        else
        {
            JsonArray emptyPortGroups = obj["portGroups"].to<JsonArray>();
        }
    }
};

// Function Registry
class FunctionRegistry
{
private:
    std::map<String, AgentFunction> functions;
    std::map<String, FunctionDefinition> definitions;

public:
    FunctionRegistry() {}

    void registerFunction(const String &functionName, const FunctionDefinition &definition, AgentFunction callback)
    {
        functions[functionName] = callback;
        definitions[functionName] = definition;
        Serial.println("Registered function: " + functionName);
    }

    AgentFunction getFunction(const String &functionName)
    {
        auto it = functions.find(functionName);
        if (it != functions.end())
        {
            return it->second;
        }
        return nullptr;
    }

    const std::map<String, FunctionDefinition> &getDefinitions() const
    {
        return definitions;
    }

    int getFunctionCount() const
    {
        return functions.size();
    }
};

// Simple UUID4 generator for agent messages
inline String agentGenerateUUID4()
{
    String uuid = "";
    const char *hexChars = "0123456789abcdef";
    for (int i = 0; i < 36; i++)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
            uuid += "-";
        else if (i == 14)
            uuid += "4";
        else if (i == 19)
            uuid += hexChars[(random(0, 4) + 8)];
        else
            uuid += hexChars[random(0, 16)];
    }
    return uuid;
}

// State definition structure
// Matches StateDefinitionInput: { name: String!, ports: [ReturnPortInput!]! }
struct StateDefinition
{
    String key;
    String name;
    JsonArray ports; // Array of ReturnPortInput definitions

    // Owned backing storage (managed by builder, transferred on build())
    DynamicJsonDocument* _portsDoc = nullptr;

    StateDefinition() {}
    StateDefinition(const String &key, const String &name)
        : key(key), name(name) {}
};

// Agent State - holds current state values and sends patches over WebSocket
class AgentState
{
private:
    String interfaceName;
    StateDefinition definition;
    DynamicJsonDocument valuesDoc;
    WebSocketsClient **wsPtr; // Pointer to Agent's ws pointer
    String *sessionId;
    int *globalRevPtr; // Points to Agent's global revision counter
    bool *sessionReadyPtr; // Points to Agent's sessionReady flag

    void sendPatch(const String &key, const JsonVariant &oldValue)
    {
        if (!wsPtr || !*wsPtr || !sessionId || sessionId->length() == 0)
            return;
        if (!sessionReadyPtr || !*sessionReadyPtr)
            return;

        (*globalRevPtr)++;

        DynamicJsonDocument patchDoc(512);
        patchDoc["type"] = "STATE_PATCH";
        patchDoc["id"] = agentGenerateUUID4();
        patchDoc["session_id"] = *sessionId;
        patchDoc["global_rev"] = *globalRevPtr;
        patchDoc["state_name"] = interfaceName;
        patchDoc["ts"] = (double)millis() / 1000.0;
        patchDoc["op"] = "replace";
        patchDoc["path"] = "/" + key;
        patchDoc["value"] = valuesDoc[key];
        if (!oldValue.isNull())
            patchDoc["old_value"] = oldValue;
        else
            patchDoc["old_value"] = (const char *)nullptr;

        String msg;
        serializeJson(patchDoc, msg);
        Serial.println("[STATE] Patch >> " + msg);
        (*wsPtr)->sendTXT(msg);
    }

public:
    AgentState(const String &iface, const StateDefinition &def, WebSocketsClient **websocket, String *session, int *globalRev, bool *sessionReady)
        : interfaceName(iface), definition(def), valuesDoc(1024), wsPtr(websocket), sessionId(session), globalRevPtr(globalRev), sessionReadyPtr(sessionReady)
    {
        valuesDoc.to<JsonObject>(); // Initialize as empty object
    }

    void setPort(const String &key, int value)
    {
        JsonVariant oldVal = valuesDoc[key];
        DynamicJsonDocument oldDoc(64);
        if (!oldVal.isNull()) oldDoc.set(oldVal);
        valuesDoc[key] = value;
        sendPatch(key, oldDoc.as<JsonVariant>());
    }

    void setPort(const String &key, float value)
    {
        JsonVariant oldVal = valuesDoc[key];
        DynamicJsonDocument oldDoc(64);
        if (!oldVal.isNull()) oldDoc.set(oldVal);
        valuesDoc[key] = value;
        sendPatch(key, oldDoc.as<JsonVariant>());
    }

    void setPort(const String &key, const String &value)
    {
        JsonVariant oldVal = valuesDoc[key];
        DynamicJsonDocument oldDoc(128);
        if (!oldVal.isNull()) oldDoc.set(oldVal);
        valuesDoc[key] = value;
        sendPatch(key, oldDoc.as<JsonVariant>());
    }

    void setPort(const String &key, bool value)
    {
        JsonVariant oldVal = valuesDoc[key];
        DynamicJsonDocument oldDoc(64);
        if (!oldVal.isNull()) oldDoc.set(oldVal);
        valuesDoc[key] = value;
        sendPatch(key, oldDoc.as<JsonVariant>());
    }

    JsonObject getSnapshot()
    {
        return valuesDoc.as<JsonObject>();
    }

    const String &getInterface() const { return interfaceName; }
    const StateDefinition &getDefinition() const { return definition; }
};

// Agent class
class Agent
{
private:
    App *app;
    String serviceIdentifier;
    String instanceId;
    String agentName;
    FunctionRegistry *registry;
    String currentAssignation;
    std::map<String, AgentState *> states;
    std::map<String, StateDefinition> stateDefinitions;
    WebSocketsClient *ws;
    String sessionId;
    int globalRev;
    bool sessionReady;

public:
    Agent(App *appInstance, const String &service, const String &instance, const String &name)
        : app(appInstance), serviceIdentifier(service), instanceId(instance), agentName(name), ws(nullptr), globalRev(0), sessionReady(false)
    {
        registry = new FunctionRegistry();
        beginSession();
    }

    ~Agent()
    {
        delete registry;
        for (auto &pair : states)
        {
            delete pair.second;
        }
    }

    void setWebSocket(WebSocketsClient *websocket)
    {
        ws = websocket;
    }

    const String &getSessionId() const
    {
        return sessionId;
    }

    void beginSession()
    {
        sessionId = agentGenerateUUID4();
        globalRev = 0;
        currentAssignation = "";
        Serial.println("[AGENT] Session ID: " + sessionId);
    }

    FunctionRegistry *getRegistry()
    {
        return registry;
    }

    void registerFunction(const String &functionName, const FunctionDefinition &definition, AgentFunction callback)
    {
        registry->registerFunction(functionName, definition, callback);
    }

    void registerState(const String &stateName, const StateDefinition &definition)
    {
        stateDefinitions[stateName] = definition;
        AgentState *state = new AgentState(stateName, definition, &ws, &sessionId, &globalRev, &sessionReady);
        states[stateName] = state;
        Serial.println("Registered state: " + stateName);
    }

    AgentState *getState(const String &stateName)
    {
        auto it = states.find(stateName);
        if (it != states.end())
        {
            return it->second;
        }
        Serial.println("[AGENT] Warning: State not found: " + stateName);
        return nullptr;
    }

    const std::map<String, StateDefinition> &getStateDefinitions() const
    {
        return stateDefinitions;
    }

    void sendSessionInit()
    {
        if (!ws)
        {
            Serial.println("[AGENT] Cannot send SESSION_INIT: no WebSocket");
            return;
        }

        DynamicJsonDocument doc(4096);
        doc["type"] = "SESSION_INIT";
        doc["id"] = agentGenerateUUID4();
        doc["session_id"] = sessionId;

        // states is a dict: { state_name: snapshot, ... }
        JsonObject statesObj = doc["states"].to<JsonObject>();
        for (const auto &pair : states)
        {
            statesObj[pair.first] = pair.second->getSnapshot();
        }

        String msg;
        serializeJson(doc, msg);
        Serial.println("[AGENT] SESSION_INIT >> " + msg);
        ws->sendTXT(msg);
        sessionReady = true;
    }

    void resetSession()
    {
        sessionReady = false;
    }

    void sendStateSnapshot()
    {
        if (!ws)
            return;

        DynamicJsonDocument doc(4096);
        doc["type"] = "STATE_SNAPSHOT";
        doc["id"] = agentGenerateUUID4();
        doc["session_id"] = sessionId;
        doc["global_rev"] = globalRev;

        // snapshots is a dict: { state_name: snapshot, ... }
        JsonObject snapshots = doc["snapshots"].to<JsonObject>();
        for (const auto &pair : states)
        {
            snapshots[pair.first] = pair.second->getSnapshot();
        }

        String msg;
        serializeJson(doc, msg);
        Serial.println("[AGENT] STATE_SNAPSHOT >> " + msg);
        ws->sendTXT(msg);
    }

    int getGlobalRev() const { return globalRev; }

    bool ensureAgent(const String &name, const JsonArray &extensions, String &errorMessage)
    {
        Serial.println("\n=== Ensuring Agent ===");

        // Step 1: Call ensureAgent to get current hash
        String ensureMutation = R"(
mutation EnsureAgent($input: AgentInput!) {
  ensureAgent(input: $input) {
    id
    instanceId
    name
    hash
  }
}
)";

        DynamicJsonDocument ensureVarsDoc(512);
        JsonObject ensureVars = ensureVarsDoc.to<JsonObject>();
        JsonObject ensureInput = ensureVars["input"].to<JsonObject>();
        ensureInput["instanceId"] = instanceId;
        ensureInput["name"] = name;

        String ensureResponse;
        if (!app->graphqlRequest(serviceIdentifier, ensureMutation, ensureVars, ensureResponse, errorMessage))
        {
            Serial.println("Failed to ensure agent: " + errorMessage);
            return false;
        }

        Serial.println("✓ Agent ensured successfully");

        // Parse the server hash from response
        DynamicJsonDocument ensureRespDoc(1024);
        deserializeJson(ensureRespDoc, ensureResponse);
        String serverHash = ensureRespDoc["data"]["ensureAgent"]["hash"] | "";
        Serial.println("Server agent hash: " + serverHash);

        // Compute local hash from the current implementation payload.
        String localHash = computeDefinitionsHash();
        Serial.println("Local definitions hash: " + localHash);

        // Step 2: Only call implementAgent if hashes differ
        if (serverHash == localHash)
        {
            Serial.println("✓ Agent is up to date, skipping implementAgent");
            return true;
        }

        Serial.println("Hashes differ, re-implementing agent...");
        return implementAgent(name, extensions, localHash, errorMessage);
    }

private:
    String computeDefinitionsHash()
    {
        // Build a deterministic string from all definitions and hash it
        const auto &definitions = registry->getDefinitions();
        String hashInput = "";
        for (const auto &pair : definitions)
        {
            hashInput += pair.first + "|";
            hashInput += pair.second.name + "|";
            hashInput += pair.second.key + "|";
            hashInput += pair.second.version + "|";
            hashInput += pair.second.kind + "|";
            hashInput += String(pair.second.stateful) + "|";
            hashInput += String(pair.second.args.size()) + "|";
            hashInput += String(pair.second.returns.size()) + ";";
        }

        // Include state definitions in hash
        for (const auto &pair : stateDefinitions)
        {
            hashInput += "S:" + pair.first + "|";
            hashInput += pair.second.name + "|";
            hashInput += pair.second.key + "|";
            hashInput += String(pair.second.ports.size()) + ";";
        }

        // Simple hash: djb2
        unsigned long hash = 5381;
        for (unsigned int i = 0; i < hashInput.length(); i++)
        {
            hash = ((hash << 5) + hash) + hashInput[i];
        }

        return String(hash, HEX);
    }

    bool implementAgent(const String &name, const JsonArray &extensions, const String &hash, String &errorMessage)
    {
        Serial.println("\n=== Implementing Agent ===");

        const auto &definitions = registry->getDefinitions();

        // Build GraphQL mutation
        String mutation = R"(
mutation ImplementAgent($input: ImplementAgentInput!) {
  implementAgent(input: $input) {
    id
    instanceId
    name
  }
}
)";

        // Build variables according to ImplementAgentInput schema
        DynamicJsonDocument varsDoc(8192);
        JsonObject vars = varsDoc.to<JsonObject>();
        JsonObject input = vars["input"].to<JsonObject>();
        input["instanceId"] = instanceId;
        input["name"] = name;
        input["hash"] = hash;

        if (!extensions.isNull())
        {
            JsonArray inputExtensions = input["extensions"].to<JsonArray>();
            for (JsonVariant extension : extensions)
            {
                inputExtensions.add(extension.as<const char *>());
            }
        }

        // Implementations
        JsonArray implementations = input["implementations"].to<JsonArray>();
        for (const auto &pair : definitions)
        {
            JsonObject implInput = implementations.add<JsonObject>();

            // interface - the function key used for dispatching
            implInput["interface"] = pair.first;

            // extension
            implInput["extension"] = "default";

            // definition (DefinitionInput)
            JsonObject definition = implInput["definition"].to<JsonObject>();
            definition["key"] = pair.second.key.length() > 0 ? pair.second.key : pair.first;
            definition["name"] = pair.second.name;
            definition["version"] = pair.second.version;
            definition["description"] = pair.second.description;
            definition["kind"] = pair.second.kind;
            definition["stateful"] = pair.second.stateful;
            definition["isDev"] = pair.second.isDev;

            // collections, interfaces, isTestFor
            if (pair.second.collections.size() > 0)
                definition["collections"] = pair.second.collections;
            else
                definition["collections"].to<JsonArray>();

            if (pair.second.interfaces.size() > 0)
                definition["interfaces"] = pair.second.interfaces;
            else
                definition["interfaces"].to<JsonArray>();

            if (pair.second.isTestFor.size() > 0)
                definition["isTestFor"] = pair.second.isTestFor;
            else
                definition["isTestFor"].to<JsonArray>();

            // args
            if (pair.second.args.size() > 0)
                definition["args"] = pair.second.args;
            else
                definition["args"].to<JsonArray>();

            // returns
            if (pair.second.returns.size() > 0)
                definition["returns"] = pair.second.returns;
            else
                definition["returns"].to<JsonArray>();

            // portGroups
            if (pair.second.portGroups.size() > 0)
                definition["portGroups"] = pair.second.portGroups;
            else
                definition["portGroups"].to<JsonArray>();

            // dependencies (AgentDependencyInput[]) - empty for now
            implInput["dependencies"].to<JsonArray>();
        }

        // States - StateImplementationInput[]: { interface: String!, definition: StateDefinitionInput! }
        JsonArray statesArr = input["states"].to<JsonArray>();
        for (const auto &pair : stateDefinitions)
        {
            JsonObject stateImpl = statesArr.add<JsonObject>();
            stateImpl["interface"] = pair.first;

            // StateDefinitionInput: { name: String!, ports: [ReturnPortInput!]! }
            JsonObject stateDef = stateImpl["definition"].to<JsonObject>();
            stateDef["name"] = pair.second.name;

            if (pair.second.ports.size() > 0)
                stateDef["ports"] = pair.second.ports;
            else
                stateDef["ports"].to<JsonArray>();
        }

        Serial.println("Implementing agent with " + String(definitions.size()) + " function(s) and " + String(stateDefinitions.size()) + " state(s)");

        String response;
        if (!app->graphqlRequest(serviceIdentifier, mutation, vars, response, errorMessage))
        {
            Serial.println("Failed to implement agent: " + errorMessage);
            return false;
        }

        Serial.println("✓ Agent implemented successfully");
        Serial.println("Response: " + response);
        return true;
    }

public:
    // Kept for backward compatibility but now just calls ensureAgent
    bool registerFunctions(String &errorMessage)
    {
        Serial.println("registerFunctions() is deprecated - functions are registered via ensureAgent/implementAgent");
        return true;
    }

    bool handleAssignment(ArkitektApp &app, const String &functionName, const String &assignation, JsonObject args)
    {
        Serial.println("→ Handling assignment: " + functionName);
        Serial.println("  Assignation ID: " + assignation);

        currentAssignation = assignation;

        AgentFunction func = registry->getFunction(functionName);
        if (func == nullptr)
        {
            Serial.println("✗ Function not found: " + functionName);
            return false;
        }

        // Create ReplyChannel for this assignment
        ReplyChannel reply(ws, assignation);

        // Execute the function — user controls yield/done/critical via ReplyChannel
        bool success = func(app, *this, args, reply);

        if (success)
        {
            Serial.println("✓ Function executed successfully");
        }
        else
        {
            Serial.println("✗ Function execution failed");
        }
        return success;
    }

    const String &getInstanceId() const
    {
        return instanceId;
    }

    const String &getAgentName() const
    {
        return agentName;
    }

    App *getApp()
    {
        return app;
    }

    void printRegistry()
    {
        Serial.println("\n=== Agent Function Registry ===");
        Serial.println("Agent: " + agentName);
        Serial.println("Instance ID: " + instanceId);
        Serial.println("Functions: " + String(registry->getFunctionCount()));

        const auto &definitions = registry->getDefinitions();
        for (const auto &pair : definitions)
        {
            Serial.println("  - " + pair.first + ": " + pair.second.description);
        }
        Serial.println("===============================\n");
    }
};

#endif // AGENT_H
