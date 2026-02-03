// Protokół (linie zakończone \n):
//  REGISTER u p
//  LOGIN u p
//  LIST_FRIENDS
//  ADD_FRIEND u2
//  LIST_GROUPS
//  CREATE_GROUP name u1 u2 ...
//  SEND_TO conv msg...     conv: u:a:b  albo g:<id>
//  LOGOUT
//
// Odpowiedzi:
//  OK ...
//  ERROR ...
//  EVENT FRIENDS a,b,c
//  EVENT GROUPINFO id name members_csv
//  EVENT MSG conv from msg...
//  EVENT PRESENCE user ONLINE|OFFLINE

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <crypt.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 5555
#define BACKLOG 50
#define MAX_LINE 4096
#define MAX_NAME 32
#define MAX_GNAME 32

#define DATA_DIR   "data"
#define USERS_DB   "data/users.db"
#define FRIENDS_DB "data/friends.db"
#define GROUPS_DB  "data/groups.db"
#define INBOX_DIR  "data/inbox"

//lista uzytkowników ktorzy są aktualnie zalogowani trzymana w pamięci RAM
typedef struct Online {
    int sock;
    char user[MAX_NAME];
    struct Online *next;
} Online;

static Online *g_online = NULL;

static pthread_mutex_t g_online_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_files_mx  = PTHREAD_MUTEX_INITIALIZER;

//id grup;żeby każda grupa miała unikalne ID nawet po restarcie serwera.
static int g_next_gid = 1;

//flaga do przerwania działania serwera;żeby móc bezpiecznie zatrzymać serwer sygnałem i wyjść z pętli
static volatile sig_atomic_t g_running = 1;

// zapamiętujemy socket nasłuchujący, żeby CTRL+C przerwało accept()
static int g_listen_sock = -1;


// wspólna funkcja do zakończenia programu z opisem błędu systemowego
static void die(const char *m){ perror(m); exit(1); }

// usuwa końce linii \n / \r z końca stringa aby dzialalo parsowanie komend
static void trim_eol(char *s){
    size_t n=strlen(s);
    while(n && (s[n-1]=='\n'||s[n-1]=='\r')) s[--n]=0;
}

//gwarantuje wysłanie całego bufora po TCP, bo send() może wysłać tylko fragment danych, więc pętla dosyła resztę aż do końca
static int send_all(int s,const char*buf,size_t n){
    size_t off=0;
    while(off<n){
        ssize_t r=send(s,buf+off,n-off,0);
        if(r<0){
            if(errno==EINTR) continue;
            return -1;
        }
        if(r==0) return -1;
        off+=(size_t)r;
    }
    return 0;
}

//formatuje odpowiedź jak printf, pilnuje zakończenia \n i wysyła całą linię przez send_all()
static int sendf(int s,const char*fmt,...){
    char out[MAX_LINE];
    va_list ap; va_start(ap,fmt);
    vsnprintf(out,sizeof(out),fmt,ap);
    va_end(ap);

    size_t n=strlen(out);
    if(!n || out[n-1]!='\n'){
        if(n+1>=sizeof(out)) return -1;
        out[n++]='\n';
        out[n]=0;
    }
    return send_all(s,out,n);
}

// ujednolica obsługę błędów — każdy błąd wysyła jako linię ERROR ..., żeby klient mógł to łatwo wykryć i wyświetlić.
static void send_error(int s,const char*fmt,...){
    char msg[MAX_LINE];
    va_list ap; va_start(ap,fmt);
    vsnprintf(msg,sizeof(msg),fmt,ap);
    va_end(ap);
    sendf(s,"ERROR %s",msg);
}

//walidacja loginu,żeby login miał sensowną długość i bezpieczne znaki
static bool valid_user(const char*u){
    size_t n=strlen(u);
    if(n<3||n>=MAX_NAME) return false;
    for(size_t i=0;i<n;i++){
        char c=u[i];
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))
            return false;
    }
    return true;
}

// walidacja nazwy grupy bez spacji zeby przekazac jako jeden argument
static bool valid_gname(const char*n){
    size_t L=strlen(n);
    if(L<3||L>=MAX_GNAME) return false;
    for(size_t i=0;i<L;i++){
        if(n[i]==' '||n[i]=='\t') return false;
    }
    return true;
}

//usuwa znaki końca linii z wiadomości i ogranicza jej długość
static void sanitize_msg(char*m){
    for(char*p=m;*p;p++) if(*p=='\n'||*p=='\r') *p=' ';
    if(strlen(m)>2000) m[2000]=0;
}

//sprawdzenie czy katalog istnieje
static void ensure_dir(const char*p){
    struct stat st;
    if(stat(p,&st)==0){
        if(!S_ISDIR(st.st_mode)){
            fprintf(stderr,"%s is not dir\n",p);
            exit(1);
        }
        return;
    }
    if(mkdir(p,0755)!=0 && errno!=EEXIST) die("mkdir");
}

//przygotowuje strukturę katalogów i plików data/ tworzy puste pliki baz, jeśli ich nie ma
static void ensure_data(void){
    ensure_dir(DATA_DIR);
    ensure_dir(INBOX_DIR);

    FILE*f;
    if((f=fopen(USERS_DB,"a"))) fclose(f);
    if((f=fopen(FRIENDS_DB,"a"))) fclose(f);
    if((f=fopen(GROUPS_DB,"a"))) fclose(f);
}

//umożliwia bezpieczne zatrzymanie serwera sygnałem, ustawiając flagę, dzięki czemu wychodzimy z accept() i zamykamy zasoby kontrolowanie
static void sig_handler(int sig){
    (void)sig;
    g_running=0;

    // zamykamy socket nasłuchujący -> blokujące accept() się “odblokuje”
    // close() jest async-signal-safe, więc wolno w handlerze.
    if(g_listen_sock != -1){
        close(g_listen_sock);
        g_listen_sock = -1;
    }
}

//dodaje po zalogowaniu uzytkownika do listy online
static void online_add(const char*user,int sock){
    Online*o=calloc(1,sizeof(*o));
    o->sock=sock;
    snprintf(o->user,sizeof(o->user),"%.31s",user);

    pthread_mutex_lock(&g_online_mx);
    o->next=g_online;
    g_online=o;
    pthread_mutex_unlock(&g_online_mx);
}

//Usuwa użytkownika z listy online, kiedy się wyloguje albo zerwie połączenie
static void online_remove_sock(int sock){
    pthread_mutex_lock(&g_online_mx);
    Online **pp=&g_online;
    while(*pp){
        if((*pp)->sock==sock){
            Online*t=*pp;
            *pp=t->next;
            free(t);
            break;
        }
        pp=&(*pp)->next;
    }
    pthread_mutex_unlock(&g_online_mx);
}

//Sprawdza, czy użytkownik jest online i jeśli tak, zwraca jego socket.
static int online_sock(const char*user){
    int s=-1;
    pthread_mutex_lock(&g_online_mx);
    for(Online*o=g_online;o;o=o->next)
        if(strcmp(o->user,user)==0){
            s=o->sock;
            break;
        }
    pthread_mutex_unlock(&g_online_mx);
    return s;
}

//aktualizuje liste znajomych online, czyta listę znajomych z pliku i wysyła do tych, którzy są aktualnie online
static void notify_friends_presence(const char*me,bool is_online){
    pthread_mutex_lock(&g_files_mx);
    FILE*f=fopen(FRIENDS_DB,"r");
    if(f){
        char u[MAX_NAME], v[MAX_NAME];
        while(fscanf(f,"%31s %31s",u,v)==2){
            if(strcmp(u,me)==0){
                int fsock = online_sock(v);
                if(fsock>=0)
                    sendf(fsock,"EVENT PRESENCE %s %s",me,is_online?"ONLINE":"OFFLINE");
            }
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_files_mx);
}

//szuka użytkownika w users.db i jeśli go znajdzie, wyciąga jego hash hasła
static bool user_hash_locked(const char*user,char*out,size_t outsz){
    FILE*f=fopen(USERS_DB,"r");
    if(!f) return false;

    char line[MAX_LINE], u[MAX_NAME], h[512];
    bool ok=false;

    while(fgets(line,sizeof(line),f)){
        trim_eol(line);
        if(sscanf(line,"%31[^:]:%511s",u,h)==2 && strcmp(u,user)==0){
            snprintf(out,outsz,"%s",h);
            ok=true;
            break;
        }
    }
    fclose(f);
    return ok;
}

//sprawdza, czy user istnieje
static bool user_exists_locked(const char*user){
    char h[8];
    return user_hash_locked(user,h,sizeof(h));
}

//tworzy losową sól w formacie SHA-512, żeby hashe haseł były różne nawet dla identycznych haseł
static void make_salt(char out[64]){
    static const char*ab="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    char r[9];
    for(int i=0;i<8;i++) r[i]=ab[rand()%64];
    r[8]=0;
    snprintf(out,64,"$6$%s$",r);
}

// rejestracja nowego użytkownika, waliduje dane, sprawdza czy user juz nie istnieje, hashuje haslo,
//zapisuje usera do bazy, tworzy plik na wiadomości offline
static bool register_user(const char*u,const char*p,char*err,size_t esz){
    if(!valid_user(u)){
        snprintf(err,esz,"Login 3..31, tylko [A-Za-z0-9_].");
        return false;
    }
    if(strlen(p)<3){
        snprintf(err,esz,"Hasło min. 3 znaki.");
        return false;
    }

    pthread_mutex_lock(&g_files_mx);

    if(user_exists_locked(u)){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Użytkownik '%.31s' już istnieje.",u);
        return false;
    }

    char salt[64];
    make_salt(salt);

    char*hash=crypt(p,salt);
    if(!hash){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Błąd hashowania hasła.");
        return false;
    }

    FILE*f=fopen(USERS_DB,"a");
    if(!f){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Nie mogę zapisać users.db.");
        return false;
    }
    fprintf(f,"%s:%s\n",u,hash);
    fclose(f);

    char path[256];
    snprintf(path,sizeof(path),"%s/%s.msg",INBOX_DIR,u);
    if((f=fopen(path,"a"))) fclose(f);

    pthread_mutex_unlock(&g_files_mx);
    return true;
}

//logowanie, sprawdzenie czy wprowadzone dane sa poprawne, czy uzytkownik istnieje
static bool check_login(const char*u,const char*p,char*err,size_t esz){
    if(!valid_user(u)){
        snprintf(err,esz,"Niepoprawny login.");
        return false;
    }

    pthread_mutex_lock(&g_files_mx);
    char stored[512];
    bool ok = user_hash_locked(u,stored,sizeof(stored));
    pthread_mutex_unlock(&g_files_mx);

    if(!ok){
        snprintf(err,esz,"Nie ma takiego użytkownika: '%.31s'.",u);
        return false;
    }

    char*calc=crypt(p,stored);
    if(!calc || strcmp(calc,stored)!=0){
        snprintf(err,esz,"Błędne hasło.");
        return false;
    }
    return true;
}


//sprawdzenie czy para osob jest znajomymi
static bool friends_are_locked(const char*a,const char*b){
    FILE*f=fopen(FRIENDS_DB,"r");
    if(!f) return false;

    char u[MAX_NAME], v[MAX_NAME];
    bool ok=false;

    while(fscanf(f,"%31s %31s",u,v)==2){
        if(strcmp(u,a)==0 && strcmp(v,b)==0){
            ok=true;
            break;
        }
    }
    fclose(f);
    return ok;
}

//zapisanie relacji znajomosci symetrycznie me-fr, fr-me
static bool add_friend(const char*me,const char*fr,char*err,size_t esz){
    if(!valid_user(fr)){
        snprintf(err,esz,"Niepoprawny login znajomego.");
        return false;
    }
    if(strcmp(me,fr)==0){
        snprintf(err,esz,"Nie możesz dodać samego siebie.");
        return false;
    }

    pthread_mutex_lock(&g_files_mx);

    if(!user_exists_locked(fr)){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Użytkownik '%.31s' nie istnieje.",fr);
        return false;
    }
    if(friends_are_locked(me,fr)){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"'%.31s' jest już w znajomych.",fr);
        return false;
    }

    FILE*f=fopen(FRIENDS_DB,"a");
    if(!f){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Nie mogę zapisać friends.db.");
        return false;
    }

    fprintf(f,"%s %s\n",me,fr);
    fprintf(f,"%s %s\n",fr,me);
    fclose(f);

    pthread_mutex_unlock(&g_files_mx);
    return true;
}

// wysyla do klienta EVENT FRIENDS i EVENT PRESENCE dla każdego znajomego, czyli liste znajomych i ich status
static void send_friends(int sock,const char*me){
    pthread_mutex_lock(&g_files_mx);

    FILE*f=fopen(FRIENDS_DB,"r");
    char csv[MAX_LINE];
    csv[0]=0;

    char list[256][MAX_NAME];
    int lcnt=0;

    if(f){
        char u[MAX_NAME], v[MAX_NAME];
        bool first=true;

        while(fscanf(f,"%31s %31s",u,v)==2){
            if(strcmp(u,me)==0){
                if(!first) strncat(csv,",",sizeof(csv)-strlen(csv)-1);
                strncat(csv,v,sizeof(csv)-strlen(csv)-1);
                first=false;

                if(lcnt<256) snprintf(list[lcnt++],MAX_NAME,"%.31s",v);
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&g_files_mx);

    // lista znajomych
    sendf(sock,"EVENT FRIENDS %s",csv);

    // statusy online/offline (na podstawie listy online w RAM)
    for(int i=0;i<lcnt;i++){
        sendf(sock,"EVENT PRESENCE %s %s",list[i], (online_sock(list[i])>=0)?"ONLINE":"OFFLINE");
    }
}


//buduje ścieżkę do pliku offline danego użytkownika
static void inbox_path(char out[256],const char*u){
    snprintf(out,256,"%s/%s.msg",INBOX_DIR,u);
}

//służy do mechanizmu “offline” — jeśli odbiorca nie ma aktywnego połączenia,
//serwer zapisuje event do pliku i dostarcza go dopiero po kolejnym loginie.
static void inbox_append_line(const char*u,const char*line){
    pthread_mutex_lock(&g_files_mx);
    char p[256];
    inbox_path(p,u);

    FILE*f=fopen(p,"a");
    if(f){
        fprintf(f,"%s\n",line);
        fclose(f);
    }
    pthread_mutex_unlock(&g_files_mx);
}

//po zalogowaniu odczytuje i wysyła wszystkie zaległe eventy i czysci plik
static void inbox_flush(int sock,const char*u){
    pthread_mutex_lock(&g_files_mx);

    char p[256];
    inbox_path(p,u);

    FILE*f=fopen(p,"r");
    if(f){
        char line[MAX_LINE];
        while(fgets(line,sizeof(line),f)){
            trim_eol(line);
            if(line[0]) sendf(sock,"%s",line);
        }
        fclose(f);

        f=fopen(p,"w");
        if(f) fclose(f);
    }

    pthread_mutex_unlock(&g_files_mx);
}


//parsuje linię id|name|members_csv, żeby łatwo czytać grupy z pliku
static bool parse_group(const char*line,int*id,char name[MAX_GNAME],char members[MAX_LINE]){
    return sscanf(line,"%d|%31[^|]|%4095[^\n]",id,name,members)==3;
}

//sprawdza członkostwo użytkownika w grupie, kopiuje members do tmp, bo strtok modyfikuje string
static bool group_has(const char*members,const char*user){
    char tmp[MAX_LINE];
    snprintf(tmp,sizeof(tmp),"%s",members);

    for(char*t=strtok(tmp,","); t; t=strtok(NULL,",")){
        if(strcmp(t,user)==0) return true;
    }
    return false;
}

//wyszukuje grupę po ID w groups.db i zwraca jej nazwę oraz członków
static bool group_get(int gid,char name[MAX_GNAME],char members[MAX_LINE]){
    pthread_mutex_lock(&g_files_mx);

    FILE*f=fopen(GROUPS_DB,"r");
    if(!f){
        pthread_mutex_unlock(&g_files_mx);
        return false;
    }

    char line[MAX_LINE];
    bool ok=false;
    int id;

    while(fgets(line,sizeof(line),f)){
        if(parse_group(line,&id,name,members) && id==gid){
            trim_eol(members);
            ok=true;
            break;
        }
    }

    fclose(f);
    pthread_mutex_unlock(&g_files_mx);
    return ok;
}

//skanuje groups.db i wysyła klientowi EVENT GROUPINFO tylko dla grup, w których użytkownik się znajduje
static void send_groups(int sock,const char*me){
    pthread_mutex_lock(&g_files_mx);

    FILE*f=fopen(GROUPS_DB,"r");
    sendf(sock,"OK LIST_GROUPS");

    if(f){
        char line[MAX_LINE], name[MAX_GNAME], members[MAX_LINE];
        int id;

        while(fgets(line,sizeof(line),f)){
            if(parse_group(line,&id,name,members)){
                trim_eol(members);

                if(group_has(members,me))
                    sendf(sock,"EVENT GROUPINFO %d %s %s",id,name,members);
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&g_files_mx);
}

//ustawia generator ID grup
static void load_next_gid(void){
    pthread_mutex_lock(&g_files_mx);

    FILE*f=fopen(GROUPS_DB,"r");
    int maxid=0;

    if(f){
        char line[MAX_LINE], name[MAX_GNAME], members[MAX_LINE];
        int id;
        while(fgets(line,sizeof(line),f))
            if(parse_group(line,&id,name,members) && id>maxid) maxid=id;
        fclose(f);
    }

    g_next_gid=maxid+1;
    pthread_mutex_unlock(&g_files_mx);
}

//tworzenie grupt, waliduje nazwę, robi unikalna liste czlonkow, sprawdza czy uzytkownicy istnieja, zapisuje do groups.db
static bool create_group(const char*name,char members[][MAX_NAME],int cnt,int*out,char*err,size_t esz){
    if(!valid_gname(name)){
        snprintf(err,esz,"Nazwa grupy 3..31, bez spacji.");
        return false;
    }

    //buduje listę unikalnych userów
    char uniq[64][MAX_NAME];
    int ucnt=0;

    for(int i=0;i<cnt;i++){
        if(!valid_user(members[i])){
            snprintf(err,esz,"Niepoprawny członek: '%.31s'.",members[i]);
            return false;
        }
        bool dup=false;
        for(int j=0;j<ucnt;j++) if(strcmp(uniq[j],members[i])==0) dup=true;
        if(dup) continue;

        snprintf(uniq[ucnt++],MAX_NAME,"%.31s",members[i]);
        if(ucnt>=64) break;
    }

    //min. 3 różne osoby
    if(ucnt<3){
        snprintf(err,esz,"Grupa musi mieć min. 3 różne osoby.");
        return false;
    }

    // sprawdza istnienie userów i zapisuje grupę
    pthread_mutex_lock(&g_files_mx);

    for(int i=0;i<ucnt;i++){
        if(!user_exists_locked(uniq[i])){
            pthread_mutex_unlock(&g_files_mx);
            snprintf(err,esz,"Użytkownik '%.31s' nie istnieje.",uniq[i]);
            return false;
        }
    }

    int gid=g_next_gid++;
    char csv[MAX_LINE];
    csv[0]=0;

    for(int i=0;i<ucnt;i++){
        if(i) strncat(csv,",",sizeof(csv)-strlen(csv)-1);
        strncat(csv,uniq[i],sizeof(csv)-strlen(csv)-1);
    }

    FILE*f=fopen(GROUPS_DB,"a");
    if(!f){
        pthread_mutex_unlock(&g_files_mx);
        snprintf(err,esz,"Nie mogę zapisać groups.db.");
        return false;
    }
    fprintf(f,"%d|%s|%s\n",gid,name,csv);
    fclose(f);

    pthread_mutex_unlock(&g_files_mx);

    *out=gid;
    return true;
}

//realizuje mechanizm online/offline: jeśli odbiorca ma socket, wysyła event natychmiast,
//a jeśli nie — zapisuje go do inboxu i dostarcza po kolejnym loginie
static void deliver(const char*to,const char*event_line){
    int s=online_sock(to);
    if(s>=0) sendf(s,"%s",event_line);
    else inbox_append_line(to,event_line);
}


typedef struct { int sock; } ThArg;

//obsługa jednego klienta w osobnym wątku
static void* client_th(void*ap){
    //pobranie socketu
    ThArg*ta=(ThArg*)ap;
    int sock=ta->sock;
    free(ta);

    // fdopen -> robi z socketa “plik”, można używać getline()
    FILE*fp=fdopen(sock,"r");
    if(!fp){
        close(sock);
        return NULL;
    }
    setvbuf(fp,NULL,_IONBF,0); // brak buforowania, natychmiastowe czytanie

    //stan sesji, czy jest zalogowany
    bool logged=false;
    char me[MAX_NAME]={0};

    char *line=NULL;
    size_t cap=0;

    //glowna petla czytania linii
    while(g_running){
        // getline() czyta pełną linię do '\n'
        ssize_t n=getline(&line,&cap,fp);
        if(n<=0) break;

        trim_eol(line);
        if(!line[0]) continue;

        // kopiujemy do bufora, bo strtok modyfikuje string
        char buf[MAX_LINE];
        snprintf(buf,sizeof(buf),"%s",line);

        // cmd = pierwsze słowo
        char *cmd=strtok(buf," ");
        // args = reszta po pierwszej spacji
        char *args=strtok(NULL,"");
        if(!cmd) continue;


        if(strcmp(cmd,"REGISTER")==0){
            char*u=args?strtok(args," "):NULL;
            char*p=u?strtok(NULL," "):NULL;

            if(!u||!p){
                send_error(sock,"Użycie: REGISTER <login> <hasło>");
                continue;
            }

            char err[256];
            if(register_user(u,p,err,sizeof(err))) sendf(sock,"OK REGISTER");
            else send_error(sock,"%s",err);
            continue;
        }

        if(strcmp(cmd,"LOGIN")==0){
            char*u=args?strtok(args," "):NULL;
            char*p=u?strtok(NULL," "):NULL;

            if(!u||!p){
                send_error(sock,"Użycie: LOGIN <login> <hasło>");
                continue;
            }
            if(logged){
                send_error(sock,"Już zalogowany jako '%.31s'.",me);
                continue;
            }

            char err[256];
            if(!check_login(u,p,err,sizeof(err))){
                send_error(sock,"%s",err);
                continue;
            }

            // logowanie:
            logged=true;
            snprintf(me,sizeof(me),"%.31s",u);

            // zapisz do listy online
            online_add(me,sock);

            // powiadom znajomych o ONLINE
            notify_friends_presence(me,true);

            // potwierdź
            sendf(sock,"OK LOGIN");

            // wyślij zaległe wiadomości offline
            inbox_flush(sock,me);
            continue;
        }

        if(strcmp(cmd,"LOGOUT")==0){
            if(!logged){
                send_error(sock,"Nie jesteś zalogowany.");
                continue;
            }

            //informuje znajomych że OFFLINE, usuwa z listy online (po sockecie)
            notify_friends_presence(me,false);
            online_remove_sock(sock);

            logged=false;
            me[0]=0;

            sendf(sock,"OK LOGOUT");
            continue;
        }

        // wszystkie komendy poniżej aktywne tylko po zalogowaniu
        if(!logged){
            send_error(sock,"Najpierw LOGIN.");
            continue;
        }

        //lista znajomych online
        if(strcmp(cmd,"LIST_FRIENDS")==0){
            sendf(sock,"OK LIST_FRIENDS");
            send_friends(sock,me);
            continue;
        }

        //wysyla swieza liste znajomych
        if(strcmp(cmd,"ADD_FRIEND")==0){
            char*fr=args?strtok(args," "):NULL;
            if(!fr){
                send_error(sock,"Użycie: ADD_FRIEND <login>");
                continue;
            }

            char err[256];
            if(add_friend(me,fr,err,sizeof(err))){
                sendf(sock,"OK ADD_FRIEND");
                // po dodaniu odśwież listę znajomych
                send_friends(sock,me);
            }else{
                send_error(sock,"%s",err);
            }
            continue;
        }

        //wysyła grupy, w których jest user
        if(strcmp(cmd,"LIST_GROUPS")==0){
            send_groups(sock,me);
            continue;
        }

        //tworzenie grupy
        if(strcmp(cmd,"CREATE_GROUP")==0){
            if(!args){
                send_error(sock,"Użycie: CREATE_GROUP <nazwa> <u1> <u2> ...");
                continue;
            }
            char *gname=strtok(args," ");
            if(!gname){
                send_error(sock,"Użycie: CREATE_GROUP <nazwa> <u1> <u2> ...");
                continue;
            }

            // lista członków: twórca (me) + podani użytkownicy
            char members[64][MAX_NAME];
            int cnt=0;

            snprintf(members[cnt++],MAX_NAME,"%.31s",me);
            for(;;){
                char*u=strtok(NULL," ");
                if(!u) break;
                if(cnt<64) snprintf(members[cnt++],MAX_NAME,"%.31s",u);
            }

            int gid;
            char err[256];

            if(!create_group(gname,members,cnt,&gid,err,sizeof(err))){
                send_error(sock,"%s",err);
                continue;
            }

            // sukces: serwer zwraca identyfikator rozmowy g:<id>
            sendf(sock,"OK CREATE_GROUP g:%d",gid);

            // wysłanie klientowi informacji o grupie (aby mógł je wyswietlic)
            char name[MAX_GNAME], mem[MAX_LINE];
            if(group_get(gid,name,mem))
                sendf(sock,"EVENT GROUPINFO %d %s %s",gid,name,mem);

            continue;
        }

        //wysyłanie wiadomości
        if(strcmp(cmd,"SEND_TO")==0){
            if(!args){
                send_error(sock,"Użycie: SEND_TO <conv_id> <msg>");
                continue;
            }
            char*conv=strtok(args," ");
            char*msg=strtok(NULL,"");

            if(!conv||!msg){
                send_error(sock,"Użycie: SEND_TO <conv_id> <msg>");
                continue;
            }

            while(*msg==' ') msg++;
            sanitize_msg(msg);

            if(!msg[0]){
                send_error(sock,"Pusta wiadomość.");
                continue;
            }

            // wiadomość prywatna: u:a:b
            if(strncmp(conv,"u:",2)==0){
                const char *ab=conv+2;
                const char *sep=strchr(ab,':');
                if(!sep){
                    send_error(sock,"Zły conv_id. Np: u:ala:ola");
                    continue;
                }

                // rozbijamy na a i b
                char a[MAX_NAME], b[MAX_NAME];
                size_t la=(size_t)(sep-ab);
                if(la>=sizeof(a)) la=sizeof(a)-1;

                memcpy(a,ab,la);
                a[la]=0;

                snprintf(b,sizeof(b),"%.31s",sep+1);

                // bezpieczeństwo: user musi być uczestnikiem rozmowy
                if(strcmp(a,me)!=0 && strcmp(b,me)!=0){
                    send_error(sock,"Nie jesteś uczestnikiem tej rozmowy.");
                    continue;
                }

                // druga strona rozmowy
                const char*other = (strcmp(a,me)==0)? b : a;

                // wymaganie: tylko ze znajomymi można pisać prywatnie
                pthread_mutex_lock(&g_files_mx);
                bool fr=friends_are_locked(me,other);
                pthread_mutex_unlock(&g_files_mx);

                if(!fr){
                    send_error(sock,"Nie jesteście znajomymi. ADD_FRIEND %.31s",other);
                    continue;
                }

                // event dla obu stron (online lub offline)
                char ev[MAX_LINE];
                snprintf(ev,sizeof(ev),"EVENT MSG %s %s %s",conv,me,msg);

                deliver(me,ev);
                deliver(other,ev);

                sendf(sock,"OK SEND_TO");
                continue;
            }

            //  wiadomość do grupy: g:<id>
            if(strncmp(conv,"g:",2)==0){
                int gid=atoi(conv+2);

                char gname[MAX_GNAME], members[MAX_LINE];
                if(!group_get(gid,gname,members)){
                    send_error(sock,"Nie ma takiej grupy: %d.",gid);
                    continue;
                }
                if(!group_has(members,me)){
                    send_error(sock,"Nie jesteś w tej grupie.");
                    continue;
                }

                // event grupowy
                char ev[MAX_LINE];
                snprintf(ev,sizeof(ev),"EVENT MSG %s %s %s",conv,me,msg);

                // wyślij do wszystkich członków (online -> socket, offline -> inbox)
                char tmp[MAX_LINE];
                snprintf(tmp,sizeof(tmp),"%s",members);

                for(char*u=strtok(tmp,","); u; u=strtok(NULL,",")){
                    deliver(u,ev);
                }

                sendf(sock,"OK SEND_TO");
                continue;
            }

            // jeśli conv_id nie pasuje do żadnego formatu:
            send_error(sock,"Nieznany conv_id. Użyj u:... albo g:...");
            continue;
        }

        // jeśli nie rozpoznano komendy:
        send_error(sock,"Nieznana komenda: %s",cmd);
    }

    // sprzątanie zasobów wątku
    free(line);

    // jeśli klient rozłączył się będąc zalogowanym -> powiadom znajomych i usuń z online
    if(logged){
        notify_friends_presence(me,false);
        online_remove_sock(sock);
    }

    fclose(fp); // zamyka socket
    return NULL;
}


// MAIN
int main(int argc,char**argv){
    int port=DEFAULT_PORT;
    if(argc>=2) port=atoi(argv[1]);

    // rand() dla soli hasła
    srand((unsigned)time(NULL)^(unsigned)getpid());

    // żeby serwer nie kończył procesu gdy send() trafi w rozłączone gniazdo
    signal(SIGPIPE, SIG_IGN);

    // SIGINT/SIGTERM przez sigaction (bez SA_RESTART),
    // dzięki temu accept() da się przerwać CTRL+C
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // przygotuj katalogi/pliki oraz wczytaj g_next_gid
    ensure_data();
    load_next_gid();

    // socket serwera
    int s=socket(AF_INET,SOCK_STREAM,0);
    if(s<0) die("socket");

    // zapisz socket nasłuchujący globalnie -> handler zamknie go na CTRL+C
    g_listen_sock = s;

    // pozwala szybko restartować serwer
    int opt=1;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    // bind na wszystkie interfejsy
    struct sockaddr_in a;
    memset(&a,0,sizeof(a));
    a.sin_family=AF_INET;
    a.sin_addr.s_addr=INADDR_ANY;
    a.sin_port=htons((uint16_t)port);

    if(bind(s,(struct sockaddr*)&a,sizeof(a))<0) die("bind");
    if(listen(s,BACKLOG)<0) die("listen");

    printf("Server listening on %d\n",port);

    // każdy klient dostaje osobny wątek
    while(g_running){
        int c=accept(s,NULL,NULL);
        if(c<0){
            // po CTRL+C handler zamknie socket -> accept może zwrócić EBADF lub EINTR
            if(!g_running) break;
            if(errno==EINTR) continue;
            if(errno==EBADF) break;
            perror("accept");
            continue;
        }

        ThArg*ta=malloc(sizeof(*ta));
        ta->sock=c;

        pthread_t th;
        if(pthread_create(&th,NULL,client_th,ta)!=0){
            perror("pthread_create");
            close(c);
            free(ta);
            continue;
        }
        pthread_detach(th); // wątek sam się posprząta po zakończeniu
    }

    if(g_listen_sock != -1){
        close(g_listen_sock);
        g_listen_sock = -1;
    }

    printf("Server stopped.\n");
    return 0;
}
