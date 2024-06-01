#define ENVIRON_FILE                    "/.bashrc"

void init_env(void);
char* getenv(const char *key);
void setenv(const char *key, const char *value);