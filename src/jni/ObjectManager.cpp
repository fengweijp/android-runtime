#include "ObjectManager.h"
#include "JniLocalRef.h"
#include "NativeScriptAssert.h"
#include "MetadataNode.h"
#include "ArgConverter.h"
#include "Util.h"
#include "V8GlobalHelpers.h"
#include "V8NativeScriptExtension.h"
#include "V8StringConstants.h"
#include <assert.h>
#include <algorithm>

using namespace v8;
using namespace std;
using namespace tns;

ObjectManager::ObjectManager()
	: m_numberOfGC(0), m_currentObjectId(0), m_cache(NewWeakGlobalRefCallback, DeleteWeakGlobalRefCallback, 1000, this)
{
	JEnv env;

	PlatformClass = env.FindClass("com/tns/Platform");
	assert(PlatformClass != nullptr);

	GET_JAVAOBJECT_BY_ID_METHOD_ID = env.GetStaticMethodID(PlatformClass, "getJavaObjectByID", "(I)Ljava/lang/Object;");
	assert(GET_JAVAOBJECT_BY_ID_METHOD_ID != nullptr);

	GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID = env.GetStaticMethodID(PlatformClass, "getorCreateJavaObjectID", "(Ljava/lang/Object;)I");
	assert(GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID != nullptr);

	MAKE_INSTANCE_WEAK_BATCH_METHOD_ID = env.GetStaticMethodID(PlatformClass, "makeInstanceWeak", "(Ljava/nio/ByteBuffer;IZ)V");
	assert(MAKE_INSTANCE_WEAK_BATCH_METHOD_ID != nullptr);

	CHECK_WEAK_OBJECTS_ARE_ALIVE_METHOD_ID = env.GetStaticMethodID(PlatformClass, "checkWeakObjectAreAlive", "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;I)V");
	assert(CHECK_WEAK_OBJECTS_ARE_ALIVE_METHOD_ID != nullptr);

	ObjectManager::instance = this;

	V8::AddGCPrologueCallback(ObjectManager::OnGcStartedStatic, kGCTypeAll);

	V8::AddGCEpilogueCallback(ObjectManager::OnGcFinishedStatic, kGCTypeAll);
}

jweak ObjectManager::GetJavaObjectByJsObjectStatic(const Handle<Object>& object)
{
	return ObjectManager::instance->GetJavaObjectByJsObject(object);
}

jweak ObjectManager::GetJavaObjectByJsObject(const Handle<Object>& object)
{
	jweak javaObject = nullptr;

	JSInstanceInfo *jsInstanceInfo = GetJSInstanceInfo(object);

	if (jsInstanceInfo != nullptr)
	{
		javaObject = GetJavaObjectByID(jsInstanceInfo->JavaObjectID);
	}

	return javaObject;
}

JSInstanceInfo* ObjectManager::GetJSInstanceInfo(const Handle<Object>& object)
{
	DEBUG_WRITE("ObjectManager::GetJSInstanceInfo: called");
	JSInstanceInfo *jsInstanceInfo = nullptr;

	Isolate* isolate = Isolate::GetCurrent();
	HandleScope handleScope(isolate);

	auto v8HiddenJS = V8StringConstants::GetHiddenJSInstance();
	auto hiddenValue = object->GetHiddenValue(v8HiddenJS);
	if (hiddenValue.IsEmpty())
	{
		//Typescript object layout has an object instance as child of the actual registered instance. checking for that
		auto prototypeObject = object->GetPrototype().As<Object>();
		if (!prototypeObject.IsEmpty())
		{
			DEBUG_WRITE("GetJSInstanceInfo: need to check prototype :%d", prototypeObject->GetIdentityHash());
			hiddenValue = prototypeObject->GetHiddenValue(v8HiddenJS);
		}
	}

	if (!hiddenValue.IsEmpty())
	{
		auto external = hiddenValue.As<External>();
		jsInstanceInfo = static_cast<JSInstanceInfo*>(external->Value());
	}
	else
	{
		DEBUG_WRITE_FATAL("Hidden JSInstanceInfo not found on object: %d", object->GetIdentityHash());
	}

	return jsInstanceInfo;
}


jweak ObjectManager::GetJavaObjectByID(uint32_t javaObjectID)
{
	jweak obj = m_cache(javaObjectID);

	return obj;
}

jobject ObjectManager::GetJavaObjectByIDImpl(uint32_t javaObjectID)
{
	JEnv env;

	jobject object = env.CallStaticObjectMethod(PlatformClass, GET_JAVAOBJECT_BY_ID_METHOD_ID, javaObjectID);
	return object;
}

void ObjectManager::UpdateCache(int objectID, jobject obj)
{
	m_cache.update(objectID, obj);
}

jclass ObjectManager::GetJavaClass(const Handle<Object>& instance)
{
	DEBUG_WRITE("GetClass called");

	JSInstanceInfo *jsInfo = GetJSInstanceInfo(instance);
	jclass clazz = jsInfo->clazz;

	return clazz;
}

void ObjectManager::SetJavaClass(const Handle<Object>& instance, jclass clazz)
{
	DEBUG_WRITE("SetClass called");

	JSInstanceInfo *jsInfo = GetJSInstanceInfo(instance);
	jsInfo->clazz = clazz;
}


int ObjectManager::GetOrCreateObjectId(jobject object)
{
	JEnv env;

	jint javaObjectID = env.CallStaticIntMethod(PlatformClass, GET_OR_CREATE_JAVA_OBJECT_ID_METHOD_ID, object);

	return javaObjectID;
}

Handle<Object> ObjectManager::GetJsObjectByJavaObjectStatic(int javaObjectID)
{
	return ObjectManager::instance->GetJsObjectByJavaObject(javaObjectID);
}

Local<Object> ObjectManager::GetJsObjectByJavaObject(int javaObjectID)
{
	auto isolate = Isolate::GetCurrent();
	EscapableHandleScope handleScope(isolate);

	auto it = idToObject.find(javaObjectID);
	if (it == idToObject.end())
	{
		return handleScope.Escape(Local<Object>());
	}

	Persistent<Object>* jsObject = it->second;

	auto localObject = Local<Object>::New(isolate, *jsObject);
	return handleScope.Escape(localObject);
}

Handle<Object> ObjectManager::CreateJSProxyInstanceStatic(jint javaObjectID, const string& typeName)
{
	return ObjectManager::instance->CreateJSProxyInstance(javaObjectID, typeName);
}

Handle<Object> ObjectManager::CreateJSProxyInstance(jint javaObjectID, const string& typeName)
{
	return CreateJSProxyInstanceHelper(javaObjectID, typeName, nullptr);
}

Handle<Object> ObjectManager::CreateJSProxyInstance(jint javaObjectID, const string& typeName, jobject instance)
{
	JEnv env;

	JniLocalRef clazz(env.GetObjectClass(instance));

	return CreateJSProxyInstanceHelper(javaObjectID, typeName, clazz);
}

Handle<Object> ObjectManager::CreateJSProxyInstanceHelper(jint javaObjectID, const string& typeName, jclass clazz)
{
	auto isolate = Isolate::GetCurrent();

	string className = (clazz != nullptr) ? GetClassName(clazz) : typeName;

	MetadataNode *node = MetadataNode::GetOrCreate(className);

	auto classProxy = node->CreateProxy(isolate);
	string name;
	auto instance = MetadataNode::CreateJSInstance(classProxy, node, Handle<Object>(), name);

	JEnv env;
	jclass claz = env.FindClass(className);
	Link(instance, javaObjectID, claz);
	return instance;
}


void ObjectManager::Link(const Handle<Object>& object, uint32_t javaObjectID, jclass clazz)
{
	auto isolate = Isolate::GetCurrent();

	DEBUG_WRITE("Linking js object: %d and java instance id: %d", object->GetIdentityHash(), javaObjectID);

	JEnv env;

	auto jsInstanceInfo = new JSInstanceInfo();
	jsInstanceInfo->JavaObjectID = javaObjectID;
	jsInstanceInfo->clazz = clazz;

	auto objectHandle = new Persistent<Object>(isolate, object);
	auto state = new ObjectWeakCallbackState(this, jsInstanceInfo, objectHandle);
	objectHandle->SetWeak(state, JSObjectWeakCallbackStatic);

	auto hiddenString = V8StringConstants::GetHiddenJSInstance();

	bool alreadyLinked = !object->GetHiddenValue(hiddenString).IsEmpty();
	ASSERT_MESSAGE(!alreadyLinked, "object should not have been linked before");

	auto hiddenValue = External::New(isolate, jsInstanceInfo);
	object->SetHiddenValue(hiddenString, hiddenValue);

	idToObject.insert(make_pair(javaObjectID, objectHandle));
}

string ObjectManager::GetClassName(jobject javaObject)
{
	JEnv env;

	JniLocalRef objectClass(env.GetObjectClass(javaObject));

	return GetClassName((jclass)objectClass);
}

string ObjectManager::GetClassName(jclass clazz)
{
	JEnv env;

	jclass javaLangClass = env.FindClass("java/lang/Class");
	jmethodID getNameId = env.GetMethodID(javaLangClass, "getName", "()Ljava/lang/String;");
	JniLocalRef javaCanonicalName(env.CallObjectMethod(clazz, getNameId));

	string className = ArgConverter::jstringToString(javaCanonicalName);

	std::replace(className.begin(), className.end(), '.', '/');

	return className;
}

void ObjectManager::JSObjectWeakCallbackStatic(const WeakCallbackData<Object, ObjectWeakCallbackState>& data)
{
	ObjectWeakCallbackState *callbackState = data.GetParameter();

	ObjectManager *thisPtr = callbackState->thisPtr;

	auto isolate = data.GetIsolate();

	thisPtr->JSObjectWeakCallback(isolate, callbackState);
}

void ObjectManager::JSObjectWeakCallback(Isolate *isolate, ObjectWeakCallbackState *callbackState)
{
	DEBUG_WRITE("JSObjectWeakCallback called");

	HandleScope handleScope(isolate);

	Persistent<Object> *po = callbackState->target;

	auto itFound = m_visitedPOs.find(po);

	if (itFound == m_visitedPOs.end())
	{
		m_visitedPOs.insert(po);

		auto obj = Local<Object>::New(isolate, *po);

		JSInstanceInfo *jsInstanceInfo = GetJSInstanceInfo(obj);
		int javaObjectID = jsInstanceInfo->JavaObjectID;

		DEBUG_WRITE("JSObjectWeakCallback objectId: %d", javaObjectID);

		bool hasImplObj = HasImplObject(isolate, obj);

		JEnv env;

		if (hasImplObj)
		{
			if (jsInstanceInfo->IsJavaObjectWeak)
			{
				m_implObjWeak.push_back(PersistentObjectIdPair(po, javaObjectID));
			}
			else
			{
				m_implObjStrong.push_back(PersistentObjectIdPair(po, javaObjectID));
				jsInstanceInfo->IsJavaObjectWeak = true;
			}

			MarkReachableObjects(isolate, obj);
		}
		else
		{
			assert(!m_markedForGC.empty());
			auto& topGCInfo = m_markedForGC.top();
			topGCInfo.markedForGC.push_back(po);
		}
	}

	po->SetWeak(callbackState, JSObjectWeakCallbackStatic);
}

string ObjectManager::JniClassPathToCanonicalName(const string& jniClassPath)
{
	std::string canonicalName;

	const char prefix = jniClassPath[0];

	std::string rest;
	int lastIndex;

	switch (prefix)
	{
		case 'L':
			canonicalName = jniClassPath.substr(1, jniClassPath.size() - 2);
			break;

		case '[':
			canonicalName = jniClassPath;
			lastIndex = canonicalName.find_last_of("[");
			rest = canonicalName.substr(lastIndex + 1);
			canonicalName = canonicalName.substr(0, lastIndex + 1);
			canonicalName.append(JniClassPathToCanonicalName(rest));
			break;

		default:
			// TODO:
			canonicalName = jniClassPath;
			break;
	}
	return canonicalName;
}

int ObjectManager::GenerateNewObjectID()
{
	const int one = 1;
	int oldValue = __sync_fetch_and_add(&m_currentObjectId, one);

	return oldValue;
}

void ObjectManager::ReleaseJSInstance(Persistent<Object> *po, JSInstanceInfo *jsInstanceInfo)
{
	DEBUG_WRITE("ReleaseJSInstance instance");

	int javaObjectID = jsInstanceInfo->JavaObjectID;

	auto it = idToObject.find(javaObjectID);
	if (it == idToObject.end())
	{
		ASSERT_FAIL("js object with id:%d not found", javaObjectID);
		APP_FAIL("ReleaseJSInstance failed");
	}

	assert(po == it->second);

	idToObject.erase(it);
	m_released.insert(po, javaObjectID);
	po->Reset();
	delete po;
	delete jsInstanceInfo;

	DEBUG_WRITE("ReleaseJSObject instance disposed. id:%d", javaObjectID);
}

void ObjectManager::ReleaseRegularObjects()
{
	Isolate *isolate = Isolate::GetCurrent();

	HandleScope handleScope(isolate);

	auto propName = String::NewFromUtf8(isolate, "t::gcNum");

	auto& topGCInfo = m_markedForGC.top();
	auto& marked = topGCInfo.markedForGC;
	int numberOfGC = topGCInfo.numberOfGC;

	for (auto po: marked)
	{
		if (m_released.contains(po))
			continue;

		auto obj = Local<Object>::New(isolate, *po);

		assert(!obj.IsEmpty());

		auto gcNum = obj->GetHiddenValue(propName);

		bool isReachableFromImplementationObject = false;

		if (!gcNum.IsEmpty())
		{
			int objGcNum = gcNum->Int32Value();

			isReachableFromImplementationObject = objGcNum >= numberOfGC;
		}

		JSInstanceInfo *jsInstanceInfo = GetJSInstanceInfo(obj);

		if (!isReachableFromImplementationObject)
		{
			if (!jsInstanceInfo->IsJavaObjectWeak)
			{
				jsInstanceInfo->IsJavaObjectWeak = true;

				ReleaseJSInstance(po, jsInstanceInfo);
			}
		}
	}

	marked.clear();
}

bool ObjectManager::HasImplObject(Isolate *isolate, const Local<Object>& obj)
{
	auto implObject = MetadataNode::GetImplementationObject(obj);

	bool hasImplObj = !implObject.IsEmpty();

	return hasImplObj;
}

void ObjectManager::MarkReachableObjects(Isolate *isolate, const Local<Object>& obj)
{
	stack< Local<Value> > s;

	s.push(obj);

	auto propName = String::NewFromUtf8(isolate, "t::gcNum");

	auto hnode = String::NewFromUtf8(isolate, "t::MetadataNode");

	assert(!m_markedForGC.empty());
	auto& topGCInfo = m_markedForGC.top();
	int numberOfGC = topGCInfo.numberOfGC;

	auto curGCNumValue = Integer::New(isolate, numberOfGC);

	while (!s.empty())
	{
		auto top = s.top();
		s.pop();

		if (top.IsEmpty() || !top->IsObject())
		{
			continue;
		}

		auto o = top.As<Object>();

		uint8_t *addr = NativeScriptExtension::GetAddress(o);
		auto itFound = m_visited.find(addr);
		if (itFound != m_visited.end())
		{
			continue;
		}
		m_visited.insert(addr);

		if (o->IsFunction())
		{
			auto func = o.As<Function>();

			int closureObjectLength;
			auto closureObjects = NativeScriptExtension::GetClosureObjects(isolate, func, &closureObjectLength);
			for (int i=0; i<closureObjectLength; i++)
			{
				auto& curV = *(closureObjects + i);
				if (!curV.IsEmpty() && curV->IsObject())
				{
					s.push(curV);
				}
			}
			NativeScriptExtension::ReleaseClosureObjects(closureObjects);
		}

		auto ns = o->GetHiddenValue(hnode);
		if (!ns.IsEmpty())
		{
			o->SetHiddenValue(propName, curGCNumValue);
		}

		auto proto = o->GetPrototype();
		if (!proto.IsEmpty() && !proto->IsNull() && !proto->IsUndefined() && proto->IsObject())
		{
			s.push(proto);
		}

		auto propNames = NativeScriptExtension::GetPropertyKeys(isolate, o);
		int len = propNames->Length();
		for (int i = 0; i < len; i++)
		{
			auto propName = propNames->Get(i);
			if (propName->IsString())
			{
				auto name = propName.As<String>();

				bool isPropDescriptor = o->HasRealNamedCallbackProperty(name);
				if (isPropDescriptor)
				{
					Handle<Value> getter;
					Handle<Value> setter;
					NativeScriptExtension::GetAssessorPair(isolate, o, name, getter, setter);

					if (!getter.IsEmpty())
					{
						int getterClosureObjectLength = 0;
						auto getterClosureObjects = NativeScriptExtension::GetClosureObjects(isolate, getter.As<Function>(), &getterClosureObjectLength);
						for (int i=0; i<getterClosureObjectLength; i++)
						{
							auto& curV = *(getterClosureObjects + i);
							if (!curV.IsEmpty() && curV->IsObject())
							{
								s.push(curV);
							}
						}
						NativeScriptExtension::ReleaseClosureObjects(getterClosureObjects);
					}

					if (!setter.IsEmpty())
					{
						int setterClosureObjectLength = 0;
						auto setterClosureObjects = NativeScriptExtension::GetClosureObjects(isolate, setter.As<Function>(), &setterClosureObjectLength);
						for (int i=0; i<setterClosureObjectLength; i++)
						{
							auto& curV = *(setterClosureObjects + i);
							if (!curV.IsEmpty() && curV->IsObject())
							{
								s.push(curV);
							}
						}
						NativeScriptExtension::ReleaseClosureObjects(setterClosureObjects);
					}
				}
				else
				{
					auto prop = o->Get(propName);

					if (!prop.IsEmpty() && prop->IsObject())
					{
						s.push(prop);
					}
				}
			}
		} // for

	} // while
}

void ObjectManager::OnGcStartedStatic(GCType type, GCCallbackFlags flags)
{
	instance->OnGcStarted(type, flags);
}

void ObjectManager::OnGcFinishedStatic(GCType type, GCCallbackFlags flags)
{
	instance->OnGcFinished(type, flags);
}

void ObjectManager::OnGcStarted(GCType type, GCCallbackFlags flags)
{
	GarbageCollectionInfo gcInfo(++m_numberOfGC);
	m_markedForGC.push(gcInfo);
}

void ObjectManager::OnGcFinished(GCType type, GCCallbackFlags flags)
{
	assert(!m_markedForGC.empty());

	ReleaseRegularObjects();

	m_markedForGC.pop();

	JEnv env;

	if (m_markedForGC.empty())
	{
		MakeRegularObjectsWeak(m_released.m_IDs, m_buff);

		MakeImplObjectsWeak(m_implObjStrong, m_buff);

		CheckWeakObjectsAreAlive(m_implObjWeak, m_buff, m_outBuff);

		m_buff.Reset();
		m_released.clear();
		m_visited.clear();
		m_visitedPOs.clear();
		m_implObjWeak.clear();
		m_implObjStrong.clear();
	}
}

void ObjectManager::MakeRegularObjectsWeak(const set<int>& instances, DirectBuffer& inputBuff)
{
	JEnv env;

	jboolean keepAsWeak = JNI_FALSE;

	for (auto javaObjectId: instances)
	{
		bool success = inputBuff.Write(javaObjectId);

		if (!success)
		{
			int length = inputBuff.Length();
			env.CallStaticVoidMethod(PlatformClass, MAKE_INSTANCE_WEAK_BATCH_METHOD_ID, (jobject)inputBuff, length, keepAsWeak);
			inputBuff.Reset();
			success = inputBuff.Write(javaObjectId);
			assert(success);
		}
	}
	int size = inputBuff.Size();
	if (size > 0)
	{
		env.CallStaticVoidMethod(PlatformClass, MAKE_INSTANCE_WEAK_BATCH_METHOD_ID, (jobject)inputBuff, size, keepAsWeak);
	}

	inputBuff.Reset();
}

void ObjectManager::MakeImplObjectsWeak(const vector<PersistentObjectIdPair>& instances, DirectBuffer& inputBuff)
{
	JEnv env;

	jboolean keepAsWeak = JNI_TRUE;

	for (const auto& poIdPair: instances)
	{
		int javaObjectId = poIdPair.javaObjectId;

		bool success = inputBuff.Write(javaObjectId);

		if (!success)
		{
			int length = inputBuff.Length();
			jboolean keepAsWeak = JNI_TRUE;
			env.CallStaticVoidMethod(PlatformClass, MAKE_INSTANCE_WEAK_BATCH_METHOD_ID, (jobject)inputBuff, length, keepAsWeak);
			inputBuff.Reset();
			success = inputBuff.Write(javaObjectId);
			assert(success);
		}
	}
	int size = inputBuff.Size();
	if (size > 0)
	{
		jboolean keepAsWeak = JNI_TRUE;
		env.CallStaticVoidMethod(PlatformClass, MAKE_INSTANCE_WEAK_BATCH_METHOD_ID, (jobject)inputBuff, size, keepAsWeak);
	}

	inputBuff.Reset();
}

void ObjectManager::CheckWeakObjectsAreAlive(const vector<PersistentObjectIdPair>& instances, DirectBuffer& inputBuff, DirectBuffer& outputBuff)
{
	JEnv env;

	for (const auto& poIdPair: instances)
	{
		int javaObjectId = poIdPair.javaObjectId;

		bool success = inputBuff.Write(javaObjectId);

		if (!success)
		{
			int length = inputBuff.Length();
			env.CallStaticVoidMethod(PlatformClass, CHECK_WEAK_OBJECTS_ARE_ALIVE_METHOD_ID, (jobject)inputBuff, (jobject)outputBuff, length);
			//
			int *released = outputBuff.GetData();
			for (int i=0; i<length; i++)
			{
				bool isReleased = *released++ != 0;

				if (isReleased)
				{
					Persistent<Object> *po = instances[i].po;
					po->Reset();
				}
			}
			//
			inputBuff.Reset();
			success = inputBuff.Write(javaObjectId);
			assert(success);
		}
	}
	int size = inputBuff.Size();
	if (size > 0)
	{
		env.CallStaticVoidMethod(PlatformClass, CHECK_WEAK_OBJECTS_ARE_ALIVE_METHOD_ID, (jobject)inputBuff, (jobject)outputBuff, size);
		int *released = outputBuff.GetData();
		for (int i=0; i<size; i++)
		{
			bool isReleased = *released++ != 0;

			if (isReleased)
			{
				Persistent<Object> *po = instances[i].po;
				po->Reset();
			}
		}
	}

}

jweak ObjectManager::NewWeakGlobalRefCallback(const int& javaObjectID, void *state)
{
	ObjectManager *objManager = reinterpret_cast<ObjectManager*>(state);

	JniLocalRef obj(objManager->GetJavaObjectByIDImpl(javaObjectID));

	JEnv env;
	jweak weakRef = env.NewWeakGlobalRef(obj);

	return weakRef;
}

void ObjectManager::DeleteWeakGlobalRefCallback(const jweak& object, void *state)
{
	JEnv env;
	env.DeleteWeakGlobalRef(object);
}


ObjectManager* ObjectManager::instance = nullptr;
