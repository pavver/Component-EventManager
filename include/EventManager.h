#pragma once

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "string.h"
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <list>
#include <stdio.h>

using namespace std;

// Терміни: Подія, Запит, Підписник, Статус події, Тип події, Підтип події
// - Подія:
//     Обєкт який описує подію яку необхідно якось обробити наприклад:
//     прийшов http запит, була натиснута кнопка, змінились показники датчика тощо.
//     Події по черзі оброблюють всі підписники які підписані на цей тип події.
// - Запит:
//     Запит це подія з однією особливістью, після обробки всіми підписниками запит
//     повертаєтся до того ж підписника який його викликав, говорити простими словами
//     то подію яка є запитом підписник може "почекати" доки всі інші підписники її не оброблять.
// - Підписник:
//     Підписуєтся на отримання всіх або конкретних подій.
// - Статус події:
//     Описує те чи подія зараз "зайнята" якимось з підписників, чи очікує наступного
//     виклику від підписника, чи данний обєкт події зовсім не задіяний...
// - Тип події:
//     Щось накшталт "основного" ідентифікатора того хто викликав подію, або хто повинен її обробляти.
//     Наприклад у http запитів тип події 1, у запитів ДО WiFi 2 тощо
// - Підтип події:
//     Описує конкретну подію, наприклад це основний тип запит до WiFi то підтип може бути
//     наприклад отримати список доступних WiWi мереж або підєднатись до WiFi мережі тощо
//
// Менеджер подій працює наступним чином.
// До менеджера підписуются підписники, в менеджер приходять події, події викликаются по черзі у всіх
// підписників або у конкретного підписника якщо він заздалегіть відомий. Також підписник може встановити
// статус події як повністью виконану (яка пройшла по всіх підписниках).
// Життєвий цикл події, подія може бути додана з будь якої частини програми і з будь якої задачі (Task)
// далі подія йде по черзі по всіх підписниках поки не пройде по всьому ланцьюжку підписників.

enum EventStatus : uint8_t
{
  /// @brief Очікує виклик від підписника
  EventWaitInvoke,

  /// @brief Подія в обробці підписником
  EventInWork,

  // Подія виконана, але подія є запитом і очікує обробки викликачем
  EventRequestWait,

  /// @brief Подія виконана
  EventDone,
};

struct EventData
{
public:
  ~EventData()
  {
    free(_value);
  }

  bool isType(uint8_t type)
  {
    return _type == type;
  }

  bool isSubtype(uint8_t subtype)
  {
    return _subtype == subtype;
  }

  void *getValue()
  {
    return _value;
  }

  void setValue(void *value)
  {
    free(_value);
    _value = value;
  }

  void setAdditionalValue(void *value)
  {
    additionalValue = value;
  }

  void *getAdditionalValue()
  {
    return additionalValue;
  }

protected:
  EventData()
  {
    _status = EventDone;
    _type = 0;
    _subtype = 0;
    _value = nullptr;
  }

  /// @brief Статус події (вхідна, вихідна, обробляєтся...)
  /// @warning тільки читати, змінювати тільки з використанням sync_mutex слассу EventManager
  EventStatus _status;

  /// @brief Основний тип події (до якого компоненту відносится)
  uint8_t _type;

  /// @brief Тип події в середині компоненту
  uint8_t _subtype;

  /// @brief Данні які необхідні події для роботи (вхідні данні або вихідні, обовязково виділені за допомогою malloc а не new)
  void *_value;

  /// @brief Доаткові данні які не будуть автоматично видалятись, використовуєтся наприклад для передачі httpd_req_t в обробник події
  void *additionalValue = nullptr;

  bool _isRequest = false;
};

struct EventRequestData : public EventData
{
  void Wait()
  {
    while (_status != EventRequestWait)
    {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  void Done()
  {
    _status = EventDone;
  }
};

struct EventDataEditable : public EventData
{
public:
  EventDataEditable() : EventData()
  {
  }

  bool isStatus(EventStatus status)
  {
    return _status == status;
  }

  void setStatus(EventStatus status)
  {
    _status = status;
  }

  void setType(uint8_t type)
  {
    _type = type;
  }

  void setSubtype(uint8_t subtype)
  {
    _subtype = subtype;
  }

  void setIsRequest(bool req)
  {
    _isRequest = req;
  }

  bool isRequest()
  {
    return _isRequest;
  }

  uint8_t getType()
  {
    return _type;
  }

  uint8_t getSubtype()
  {
    return _subtype;
  }

  uint16_t countSubscribers = 0;
};

class Subscriber
{
public:
  /// @return повертає наступну подію або nullptr якщо подій нема
  EventData *Next();

  /// @brief Подія виконана
  void Done();

  const uint8_t _type;
  const uint8_t _subtype;

  const char *getName()
  {
    return _name;
  }

protected:
  Subscriber(const char *name, uint8_t type = 0, uint8_t subtype = 0)
      : _type(type),
        _subtype(subtype)
  {
    snprintf(_name, 12, "%s", name);
  }

  char _name[13];
  EventData *CurrentEvent = nullptr;
  list<EventData *> EventsQueue;
};

class SubscriberEditable : Subscriber
{
public:
  SubscriberEditable(const char *name, uint8_t type = 0, uint8_t subtype = 0)
      : Subscriber(name, type, subtype)
  {
  }

  ~SubscriberEditable();

  void TryAddEvent(EventData *event);
};

class EventManager
{
public:
  /// @brief
  /// @param queueLength Розмір черги подій
  EventManager(uint8_t queueLength) : _queueLength(queueLength)
  {
    sync_mutex = xSemaphoreCreateMutex();
    EventsQueue = (EventDataEditable **)malloc(sizeof(EventDataEditable *) * _queueLength);
    for (uint8_t i = 0; i < _queueLength; i++)
      EventsQueue[i] = new EventDataEditable();
  }

  ~EventManager()
  {
    for (size_t i = 0; i < _queueLength; i++)
      delete EventsQueue[i];
    free(EventsQueue);
  }

  uint8_t getQueueLength()
  {
    return _queueLength;
  }

  /// @brief Додати подію в чергу, якщо черга переповнена то подія не буде додана
  /// @param type тип додаваємої події (обовязково значення більше нуля)
  /// @param subtype підтип додаваємої події (обовязково значення більше нуля)
  /// @param inputData данні додаваємої події (обєкт має бути ініційований за допомогою malloc а не new)
  /// @param isRequest якщо true то буде очікуватись обробка та повертатись відповідь з value
  /// @param additionalValue додаткові данні які не будуть автоматично видалятись, використовуєтся наприклад для передачі httpd_req_t в обробник події
  /// @return якщо подія була додана то повертає true інакше false
  EventData *AddEvent(uint8_t type, uint8_t subtype, void *inputData = nullptr, bool isRequest = false, void *additionalValue = nullptr);

  /// @brief Підписатись на отримання подій
  /// @param type Тип подій які отримувати (0 якщо всі)
  /// @param subtype Підтип подій які отримувати (0 якщо всі)
  Subscriber *Subscribe(const char *name, uint8_t type, uint8_t subtype);

  /// @brief Відписатися від отримання подій
  void UnSubscribe(Subscriber *subscriber);

  bool sync_lock()
  {
    return sync_mutex ? (xSemaphoreTake(sync_mutex, portMAX_DELAY) == pdTRUE) : false;
  }

  void sync_unlock()
  {
    xSemaphoreGive(sync_mutex);
  }

private:
  /// @brief Розмір черги
  const uint8_t _queueLength;
  EventDataEditable **EventsQueue;

  list<SubscriberEditable *> Subscribers;

  SemaphoreHandle_t sync_mutex = nullptr;
};

/// @brief Керування подіями системи
extern EventManager *eventManager;
