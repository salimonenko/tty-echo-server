/* Компиляция: g++ -O2 -std=c++17 -o tty_echo tty_echo.cpp
   Запуск:     sudo ./tty_echo /dev/pts/1

где /dev/pts/1 - это имя открытого терминала (консоли). Определяется при помощи команды tty. Хотя, будет работать и без этого параметра.
*/

/********   0. ОПИСАТЕЛЬНАЯ ЧАСТЬ  ********/

// Подключаем заголовки для низкоуровневых POSIX-функций (read/write/close и т.д.)
#include <unistd.h>
// Подключаем заголовок для атомарных операций — нужен, чтобы безопасно менять флаг stop
// из обработчика сигнала и проверять его в основном цикле.
#include <atomic>
// Заголовок для работы с сигналами (sigaction, struct sigaction и т.п.)
#include <csignal>
// Для memset (если понадобится обнулять структуры) и других базовых функций
#include <cstring>
// iostream нужен для вывода ошибок через cerr.
#include <iostream>

#include <string>
#include <vector>
#include <sstream>

#include <sys/ioctl.h> // Для проверки очереди ядра: есть ли в ней что-то
#include <termios.h>

#include <algorithm>

using namespace std;

// Описываем прототипы функций
char *open_read_file(char *name);
string trim(const string& str);
void write_custom(char *buffer_voc, char *buffer);
string do_command(const string str, const string reg);

/********   1. ОПРЕДЕЛЯЕМ ОБРАБОТЧИК СИГНАЛА SIGINT (для перехвата Ctrl+C)  ********/
// Глобальный атомарный флаг остановки.
// atomic<bool> гарантирует, что запись и чтение этой переменной безопасны даже если одна сторона (обработчик сигнала) пишет, а другая (цикл) читает.
// Изначально false — программа не должна останавливаться. Как только станет true, будет останов
atomic<bool> stop(false);

// Обработчик сигнала.
// Когда приходит SIGINT (Ctrl+C), ядро вызывает эту функцию в произвольный момент.
// Параметр int — номер сигнала (в данном случае не нужен).
void handler(int) {
    // Устанавливаем флаг «пора останавливаться» атомарно.
    // memory_order_relaxed — самая лёгкая модель памяти: нам важно только, чтобы значение стало true целиком, без «половинных» состояний.
    stop.store(true, memory_order_relaxed);
}



int main() {
/********   2. ОБЪЯВЛЯЕМ СТРУКТУРУ SIGACTION — ОНА ОПИСЫВАЕТ, КАК ОБРАБАТЫВАТЬ СИГНАЛ   ********/
    struct sigaction sa{};
    // Обнуляем структуру, чтобы в ней не было мусора.
    // В C++ агрегатная инициализация {} уже обнуляет поля, но надежнее через явный memset (при исп. POSIX API)
    memset(&sa, 0, sizeof(sa));

    // Указываем, какую функцию-обработчик вызывать при получении сигнала.
    sa.sa_handler = handler;

    // Очищаем маску сигналов: во время выполнения handler не должны блокироваться
    // другие сигналы (по умолчанию).
    sigemptyset(&sa.sa_mask);

    // Флаги поведения.
    // 0 означает: НЕ использовать SA_RESTART.
    // Это критически важно: если бы стоял SA_RESTART, системный вызов read() автоматически перезапускался бы после прерывания сигналом.
    // Нам нужно, чтобы read вернул -1 и установил errno = EINTR — тогда мы сможем корректно проверить флаг stop и выйти из цикла.
    sa.sa_flags = 0;

/********   3. РЕГИСТРИРУЕМ ОБРАБОТЧИК ДЛЯ СИГНАЛА SIGINT (ИМЕННО ЕГО ПОСЫЛАЕТ CTRL+C)   ********/
    // Если регистрация не удалась — выводим ошибку и завершаем программу.
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        perror("Error: sigaction");
        return 1;
    }

/********   4. ОСНОВНОЙ ЦИКЛ РАБОТАЕТ, ПОКА ФЛАГ stop == false   ********/
// Буфер для чтения данных из терминала. Размер 4096 байт — типичный размер страницы памяти
    char buffer_tmp[5];
    char buffer[4096];
    char tb[4095]; // Чтобы не было предупреждений

    int n_SUM = 0;

    // Как только сработает Ctrl+C, handler установит stop = true и программа выйдет.
    while (!stop.load(memory_order_relaxed)) {
        // Читаем данные из стандартного ввода (STDIN_FILENO = 0).
        // Это тот самый ввод, который пользователь набирает в терминале.
        // read вернёт:
        //  > 0 — количество прочитанных байт,
        //   0 — конец ввода (EOF),
        //  -1 — ошибка (тогда смотрим errno).
        memset (buffer_tmp, 0, sizeof(buffer_tmp)); // Иначе в буфере сохраняется предыдущее значение

        if (n_SUM == 0) {
            cout << "Введите команду (для выхода нажмите Ctrl+C): "  << endl;
        }


        ssize_t n = read(STDIN_FILENO, buffer_tmp, sizeof(buffer_tmp));
        n_SUM += n;

// Если read вернул отрицательный результат — выходим из цикла. 0 обычно означает EOF (например, Ctrl+D в терминале).
        // Отрицательное значение — ошибка (включая EINTR, который обработаем ниже).
            // Флаг stop всё равно будет проверен в условии while.
            if (n == -1) {
                if (errno == EINTR) { // Если ошибка именно EINTR (прервано сигналом), можно пока продолжить.
                    perror("EINTR");
                    continue;
                } else {
                    perror("Error: read"); // Для любых других ошибок - вывести сообщение
                    break;
                }
            }

            strncat(tb, buffer_tmp, n); // Накапливаем байты, прочитанные за 1 итерацию цикла

//cout << "Прочитали n: " << n << " символов; tb: " <<(tb)<< "%" << "buffer_tmp: " << buffer_tmp << "%" << endl;

            if ((buffer_tmp[0] == '\n' || n == 0) && n_SUM <= 1) { // Если нажали Enter или Ctrl+D БЕЗ ввода других символов
                cout << "Вводить пустые строки недопустимо. Введите правильную команду. " << endl;
                continue;
            }

            buffer_tmp[n] = '\0'; // По идее, необязательно

            if (n > 0) {
                if (n > sizeof(buffer) - 1) {
                    cout << "Длина команды не может быть более " << (sizeof(buffer) - 1) << endl;
                    n_SUM = 0;
                    break;
                }

            if (n < sizeof(buffer_tmp) || buffer_tmp[n-1] == '\n') { // Значит, считаны точно последние данные, введенные с терминала. В т.ч., если была нажата Enter.

                memset (buffer, 0, sizeof(buffer));
                strncat(buffer, tb, sizeof(tb));
                memset (tb, 0, sizeof(tb));
                memset (buffer_tmp, 0, sizeof(buffer_tmp));

//cout << "  n_FIN1: " << n << ", sizeof(buffer_tmp): " <<(sizeof(buffer_tmp)) << ", buffer_tmp: "<< buffer_tmp << "|" << endl;

                } else { // Такое бывает после нажатия Ctrl+D и при числе введенных на терминале символов, кратных числу sizeof(buffer_tmp[5]). Здесь приходится разбираться подробнее, таки все байты прочитаны командой read или не все.

int bytes_in_queue = 0; // bytes_in_queue — сколько байт (введенных на терминале) уже есть в очереди ядра. правда, это не все, что введено, а лишь то, что УЖЕ ПОПАЛО в очередь ядра, т.е. не заблокировано.
if (ioctl(STDIN_FILENO, TIOCINQ, &bytes_in_queue) == 0) { // успешно
    if (bytes_in_queue > 0) { // Значит, еще точно не все байты прочитаны ядром из тех, что были введены на терминале

//cout << "  n_FIN2: " << n_SUM << ", sizeof(buffer_tmp): " <<(sizeof(buffer_tmp)) << ", buffer_tmp: "<< buffer_tmp << "|" << endl;

                    if (buffer_tmp[n-1] != '\n' ) {
                        continue; // Продолжаем, т.к. есть еще байты в очереди, введенные на терминале пользователем, но не прочитанные командой read
                    }

    } else {
//cout << "  n_FIN3: " << n_SUM << ", sizeof(buffer_tmp): " <<(sizeof(buffer_tmp)) << ", buffer_tmp: "<< buffer_tmp << "|" << endl;

        memset (buffer, 0, sizeof(buffer));
        strncat(buffer, tb, sizeof(tb));
        memset (tb, 0, sizeof(tb));
        memset (buffer_tmp, 0, sizeof(buffer_tmp));
    }

} else {
    if (errno == ENOTTY) {
        // Это не терминал (например, при перенаправлении ввода или для некоторых GUI-терминалов): TIOCINQ неприменим
        bytes_in_queue = 0;              // безопасное значение по умолчанию
        // cerr << "TIOCINQ недоступен: stdin не является терминалом\n";
    } else { // Настоящая ошибка
        cerr << "ioctl(TIOCINQ) failed: " << strerror(errno) << "\n";
        bytes_in_queue = 0;             // или другая стратегия
        perror("Ошибка ioctl");
        break;
    }
}
                }

            }


/********   5. ВЫВОДИМ СТАНДАРТНОЕ УВЕДОМЛЕНИЕ (перед ответом сервера):  ********/
        const char* label = "Введенная команда (дублируем):\n"; // Предварительное уведомление
        size_t label_len = strlen(label);

        ssize_t w = write(STDOUT_FILENO, label, label_len); // Вывод собственно ответа сервера
        // Если вывод был прерван сигналом — пробуем снова
        while (w == -1 && errno == EINTR) {
            w = write(STDOUT_FILENO, label, label_len);
        }
        if (w == -1) {
            perror("write (label)");
            break;
        }

/********   6. ЧИТАЕМ ФАЙЛ-СЛОВАРЬ ОЖИДАНИЙ:  ********/
        string name_s = "vocabulary.txt"; // Имя файла-словаря
        char* name = (char*)name_s.c_str();

        char* buffer_voc = open_read_file(name); // Читаем файл-словарь

        vector<string> lines;
        string line;

        // Создаём поток из строки
        stringstream ss(buffer_voc);

        // Извлекаем по одной строке (до \n)
        while (getline(ss, line)) {
            lines.push_back(line);
        }

        // Вывод результата для проверки
        /* cout << "Получено строк из файла-словаря: " << lines.size() << "\n\n";
        for (size_t i = 0; i < lines.size(); ++i) {
            cout << "[" << i << "] \"" << lines[i] << "\"\n";
        } */


/********   7. ВЫВОДИМ ОТВЕТ-ОЖИДАНИЕ СЕРВЕРА ПОРЦИЯМИ:  ********/
// Пишем введенные и прочитанные символы (ожидание для команды типа AT***) обратно в стандартный вывод (STDOUT_FILENO = 1). Это для проверки того, что введенная команда коректно воспринята сервером
        ssize_t written = 0;

        while (written < n_SUM) {
            // Пытаемся записать в терминал (т.е. вывести) очередную часть данных, только что веденных в нем.
            ssize_t w = write(STDOUT_FILENO, buffer + written, n_SUM - written);

            write_custom(buffer_voc, buffer); // Вывод на экран (в консоль)

            // Если write вернул <= 0, анализируем причину.
            if (w == -1) {
                if (errno == EINTR){ // EINTR означает, что запись была прервана сигналом — пробуем снова, чтобы вывести оставшиеся символы полностью.
                    continue;
                } else {
                    perror("Error: write"); // Для любых других ошибок - вывести сообщение
                    break;
                }
            }
            // Увеличиваем счётчик записанных байт.
            written += w;
        }

        n_SUM = 0;
    }

    // Программа завершается штатно.
    return 0;
}


/********   ФУНКЦИИ:  ********/

// 1. Функция, читающая файл-словарь
char *open_read_file(char *file_name) {
    #define BS 64
    size_t bytes_read;
    char buf[BS+1], *str;
    ssize_t count;
    int i = 0;

    FILE *fd;
    if ((fd = fopen(file_name, "rt")) == NULL) {
        printf("Ошибка при открытии файла-словаря.\n");
        exit(1);
    }

    memset (buf, 0, sizeof(buf)); // Вначале выделяем память для buf, с учетом ее размера (определяя пока нулевые значения). А ПОТОМ уже выделяем память для str. Иначе программа может начать работать некорректно в силу того, что часть данных из str может быть использована для buf

    // А теперь выделяем BS+1 байтов памяти, для начала
    if ((str = (char*)malloc(BS+1)) == NULL) {
        perror("Allocation error1.");
        exit (0);
    }
    memset (str, 0, sizeof(str));

    // Читаем файл-словарь до тех пор, пока не дойдем до его конца
    while (!feof(fd)) {
        i++;
        // Считываем не более, чем BS байтов из файла за 1 итерацию цикла
        if (fgets(buf, BS, fd))
            strncat(str, buf, BS);
        // Добавляем еще BS байтов памяти после того, как записали считанные байты в массив str
        if ((str = (char*)realloc(str, (1+i)*BS+1)) == NULL) {
            perror("Allocation error2.");
        exit (0);
        }
    }

if (str == ""){
    printf("Not to read from file");
}

fclose(fd);

return str;
}

// 2. Функция убирает основные пробельные символы из начала и конца строки
string trim(const string& str1) {
    // Набор символов, которые считаем «пробельными»
    const string whitespace = " \t\n\r\f\v";

    // Ищем первый символ, который НЕ из whitespace
    size_t start = str1.find_first_not_of(whitespace);
    if (start == string::npos) {
        // Строка пустая или состоит только из пробелов
        return "";
    }

    // Ищем последний символ, который НЕ из whitespace
    size_t end = str1.find_last_not_of(whitespace);

    // Возвращаем подстроку между ними
    return str1.substr(start, end - start + 1);
}



// 3. Функция выводит на экран в консоль результат выполнения команды
void write_custom(char *buffer_voc, char *buffer) {

    istringstream iss(buffer_voc);

    string token1;
    string token2;

    int flag = 0; // Флаг присутствия команды в файле-словаре
    string rezult;

        while (getline(iss, token1, '=')) { // Читаем 1-й токен (до знака =)

            getline(iss, token2); // Читаем 2-й токен (после знака = и до конца строки, т.е. до символа \n)
//            cout << "Исходно: !" << token1 << "!"<< trim(token2)<<"!"<< trim(buffer)<< "!" << endl;

            rezult = do_command(trim(buffer), trim(token1)); // Эмулируем результат "выполнения" команды

            if (rezult == "0") { // Если нет соответствия введенной команды с рег. выраж.
                continue;

            } else if (rezult == "1") { // Есть соответствие
                flag = 1;
                break;

            } else { // В случае ошибки
                flag = -1;
            }

            cout << "Возможная комманда: " << token1 << "; flag=" << flag << endl;
        }

        if (flag == 0) {
            cout << "  Неизвестная команда (в словаре ее нет)" << endl << endl;
        } else  if (flag == 1) {
            cout << "Такая команда содержится с словаре. Эмуляция ее выполнения:" << endl << "----------------" << endl << token2  << endl << "----------------" << endl << endl;
        } else { // В случае ошибки
            cout << rezult << endl;
        }
}

// 4. Функция-эмулятор выполнения команд
string do_command(const string str, const string reg) {

char ch[] = {'[', ']'};

int p1 = count(str.begin(), str.end(), ch[0]);
int p2 = count(str.begin(), str.end(), ch[1]);

    if (p1 == 0 && p2 == 0) {
        if (str == reg) { // Простое сравнение введенной и словарной команд
            return "1";
        } else {
            return "0";
        }

    } else if (p1 == 1 && p2 == 1) {

        int p1_str = str.find("[");
        int p2_str = str.find("]");

        if (p1_str > p2_str) {
            return "Неверный порядок скобок [ и ] во введенной строке";
        }

        string expression_str = str.string::substr(p1_str + 1, p2_str - p1_str - 1);

        int p1_reg = reg.find("[");
        int p2_reg = reg.find("]");
        string expression_reg = reg.string::substr(p1_reg + 1, p2_reg - p1_reg -1);

        int n_str = count(expression_str.begin(), expression_str.end(), ',');
        int n_reg = count(expression_reg.begin(), expression_reg.end(), ',');

        string flag = "0";
        if (n_str == 1 && n_reg == 1) { // Есть соответствие рег. выражению в скобках [ ]

            string str1 = str.string::substr(0, p1_str) + str.string::substr(p2_str + 1);
            string reg1 = reg.string::substr(0, p1_reg) + reg.string::substr(p2_reg + 1);

            if (str1 == reg1) { // Если есть соответствие между строкой и рег. выражением после удаления содержимого со скобками [ ]
                flag = "1";
            }

        }

        return flag;
    } else {
        return "Числа каждой из скобок во введенной строке [ ] не равны 1";
    }
}

