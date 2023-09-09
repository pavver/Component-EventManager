#include "EventManager.h"

/* char *EventData::ToString()
{
  cJSON *json = cJSON_CreateObject();

  cJSON *status = cJSON_CreateString(EventStatusToString(_status));
  cJSON_AddItemToObject(json, "status", status);

  cJSON *type = cJSON_CreateNumber(_type);
  cJSON_AddItemToObject(json, "type", type);

  cJSON *subtype = cJSON_CreateNumber(_subtype);
  cJSON_AddItemToObject(json, "subtype", subtype);

  char *ret = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  return ret;
} */

EventData *Subscriber::Next()
{
  if (CurrentEvent != nullptr)
    return CurrentEvent;
  if (EventsQueue.empty())
    return nullptr;

  eventManager->sync_lock();

  for (auto it = EventsQueue.begin(); it != EventsQueue.end(); ++it)
  {
    EventDataEditable *event = (EventDataEditable *)*it;

    if (!event->isStatus(EventWaitInvoke))
      continue;

    // ESP_LOGI("EventManager", "%s", event->ToString());

    event->setStatus(EventInWork);
    CurrentEvent = event;
    EventsQueue.erase(it);

    eventManager->sync_unlock();
    return CurrentEvent;
  }

  eventManager->sync_unlock();
  return nullptr;
}

void Subscriber::Done()
{
  if (CurrentEvent == nullptr)
    return;

  eventManager->sync_lock();

  EventDataEditable *e = (EventDataEditable *)CurrentEvent;
  e->countSubscribers--;
  e->setStatus(e->countSubscribers > 0 ? EventWaitInvoke : e->isRequest() ? EventRequestWait
                                                                          : EventDone);
  CurrentEvent = nullptr;

  eventManager->sync_unlock();
}

SubscriberEditable::~SubscriberEditable()
{
  Done();

  if (EventsQueue.empty())
    return;

  eventManager->sync_lock();

  for (auto it = EventsQueue.begin(); it != EventsQueue.end(); ++it)
  {
    EventDataEditable *e = (EventDataEditable *)*it;
    e->countSubscribers--;
  }

  eventManager->sync_unlock();
}

void SubscriberEditable::TryAddEvent(EventData *event)
{
  bool add = event->isType(_type) ? (_subtype == 0 || event->isSubtype(_subtype)) : _type == 0;

  if (!add)
    return;

  EventDataEditable *e = (EventDataEditable *)event;
  e->countSubscribers++;
  EventsQueue.push_back(event);
}

Subscriber *EventManager::Subscribe(const char *name, uint8_t type = 0, uint8_t subtype = 0)
{
  SubscriberEditable *subscriber = new SubscriberEditable(name, type, subtype);

  sync_lock();

  Subscribers.push_back(subscriber);

  sync_unlock();

  return (Subscriber *)subscriber;
}

void EventManager::UnSubscribe(Subscriber *subscriber)
{
  SubscriberEditable *subs = (SubscriberEditable *)subscriber;

  sync_lock();

  Subscribers.remove(subs);

  delete subs;

  sync_unlock();
}

EventData *EventManager::AddEvent(uint8_t type, uint8_t subtype, void *inputData, bool isRequest)
{
  EventDataEditable *e = nullptr;

  sync_lock();

  for (uint8_t i = 0; i < _queueLength; i++)
  {
    if (EventsQueue[i]->isStatus(EventDone))
    {
      e = EventsQueue[i];
      break;
    }
  }

  if (e == nullptr)
  {
    sync_unlock();
    return nullptr;
  }

  e->setType(type);
  e->setSubtype(subtype);
  e->setValue(inputData);
  e->setStatus(EventWaitInvoke);
  e->setIsRequest(isRequest);

  for (auto it = Subscribers.begin(); it != Subscribers.end(); ++it)
  {
    (*it)->TryAddEvent(e);
  }

  if (e->countSubscribers == 0)
    e->setStatus(EventDone);

  sync_unlock();

  return e;
}
