#define _GNU_SOURCE

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <SDL.h>
#include <SDL_mixer.h>
#include <math.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

#define W 128
#define H 128

#define MAX_HP 1000

#define CLAMP(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

#ifdef DEBUG
    #define check(x)\
    if (!(x)) {\
        if (errno) {\
            flockfile(stderr);\
            fprintf(stderr, "%s:%d " , __FILE__, __LINE__);\
            perror(NULL);\
            funlockfile(stderr);\
        }\
        errno = 0;\
        goto error;\
    }
#else
    #define check(x) if (!(x)) { errno = 0; goto error; }
#endif

typedef enum {
    PROGRAM_LIMIT = 500,
} limits_enum;

typedef enum {
    Defend = 0,
    Attack,
    Gamble,
} program_result_enum;

typedef struct program_result_lut {
    program_result_enum program_one_intent;
    program_result_enum program_two_intent;

    int program_one_damage_taken;
    int program_two_damage_taken;
} program_result_lut;

program_result_lut result_table[] = {
    {Attack, Attack, 5, 5},
    {Defend, Attack, 1, 0},
    {Attack, Defend, 0, 1},
    {Defend, Defend, 0, 0},
};

const char *program_result_enum_strings[] = {
    "Defend",
    "Attack",
    "Gamble",
};
const char *program_result_enum_strings_lower[] = {
    "defend",
    "attack",
    "gamble",
};

typedef enum {
    LT,
    GT,
    EQ,
    ER,
    FLAGS_COUNT,
} flags_enum;

typedef enum {
    R0 = 0,
    R1,
    R2,

    C0,
    C1,

    E0,
    E1,

    I0,
    I1,

    O0,

    T0,

    REGISTERS_COUNT,
} registers_enum;

const char *registers_enum_strings[] = {
    "R0",
    "R1",
    "R2",

    "C0",
    "C1",

    "E0",
    "E1",

    "I0",
    "I1",

    "O0",

    "T0",

    "REGISTERS_COUNT",
};

typedef enum {
    INC = 0,
    DEC,

    INCEQ,
    DECEQ,

    ADD,
    SUB,
    MUL,

    STORE,
    MOVE,

    LABEL,
    JMP,
    JMPEQ,
    JMPNE,
    JMPGT,
    JMPLT,

    CMP,

    RET,

    OPS_COUNT,
} ops_enum;

const char *ops_enum_strings[] = {
    "INC",
    "DEC",

    "INCEQ",
    "DECEQ",

    "ADD",
    "SUB",
    "MUL",

    "STORE",
    "MOVE",

    "LABEL",
    "JMP",
    "JMPEQ",
    "JMPNE",
    "JMPGT",
    "JMPLT",

    "CMP",
    "RET",

    "OPS_COUNT",
};


typedef struct program {
    int hp;
    const char *name;
    char *asmcode;
    size_t asmcode_len;
    int *bytecode;
    size_t bytecode_len;
    int labels[10];

    program_result_enum result;
    int strength;
} program;

#define PROGRAM_COUNT 2
program user_program[PROGRAM_COUNT];

int flags[FLAGS_COUNT];
int registers[REGISTERS_COUNT];

typedef struct software_texture
{
    SDL_Surface *surface;
    int w, h;
    Uint32 *pixels;
} software_texture;

static software_texture cpu_texture;
static SDL_Texture *gpu_texture;
static SDL_Renderer *renderer;

static int done()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            return 1;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
                return 1;
            }
            break;
        }
    }
    return 0;
}

static int randrange (int lower, int upper)
{
    return (rand() % (upper - lower + 1)) + lower;
}

extern inline double dist(double x1, double y1, double x2, double y2)
{
    return sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2));
}

int read_code(const char *path, program *user_program)
{
    size_t len = -1;
    user_program->asmcode = NULL;
    user_program->asmcode_len = -1;
    FILE *fp = fopen(path, "r");

    if (fp != NULL) {
        if (fseek(fp, 0L, SEEK_END) == 0) {
            long bufsize = ftell(fp);
            if (bufsize == -1) { /* Error */ }
            user_program->asmcode = malloc(sizeof(char) * (bufsize + 1));
            user_program->bytecode = malloc(sizeof(int) * (bufsize + 1));
            if (fseek(fp, 0L, SEEK_SET) != 0) { /* Error */ }
            len = fread((char*)user_program->asmcode, sizeof(char), bufsize, fp);
            if (len == 0) {
                fputs("Error reading file", stderr);
            } else {
                user_program->asmcode[++len] = '\0'; /* Just to be safe. */
                user_program->name = basename(path);
            }
            user_program->asmcode_len = len;
        }
        fclose(fp);
    }
    return user_program->asmcode_len;
}

static char *skip_space(char *p, char *end)
{
    for(;p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'); ++p);
    return p <= end ? p : NULL;
}

static char *next_space(char *p, char *end)
{
    for(;p < end && !(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'); ++p);
    return p <= end ? p : NULL;
}

static char *skip_comma(char *p, char *end)
{
    for(;p < end && *p == ','; ++p);
    return p <= end ? p : NULL;
}

static char *next_eol(char *p, char *end)
{
    for(;p < end && *p != '\n'; ++p);
    return p <= end ? p : NULL;
}

int is_opcode(char *p, char *end, ops_enum opcode)
{
    const char *opcode_string = ops_enum_strings[opcode];
    size_t n = strlen(opcode_string);

    char *t = next_space(p, end);
    int oplen = t - p;

    if (n != oplen) return 0; 
    if (strncmp(p, opcode_string, oplen) == 0) return 1;
    return 0;
}

int next_opcode(char *p, char *end)
{
    for (int i = 0; i < OPS_COUNT; i++) {
        if (is_opcode(p, end, i)) {
            return i;
        }
    }
    return -1;
}

registers_enum get_register(char *t, char *end)
{
    for (int i = 0; i < REGISTERS_COUNT; i++) {

        size_t slen = strlen(registers_enum_strings[i]);
        if (slen < (end - t)) {
            if (memcmp(t, registers_enum_strings[i], slen) == 0) {
                return i;
            }
        }
    }
    return -1;
}

void program_set_bytecode(program *p, int code)
{
    p->bytecode[p->bytecode_len++] = code;
}

void parse_code(program *user_program)
{
    char *p = user_program->asmcode;
    char *end = p + user_program->asmcode_len;
    char *t;
    char *t_end;
    int v;
    char buf[64];
    ops_enum opcode;
    registers_enum r;
    user_program->bytecode_len = 0;
    for (;;) {

        memset(buf, 0, sizeof(buf));
        check((t = skip_space(p, end)))
        opcode = next_opcode(t, end);
        if (opcode == -1) break;
        switch(opcode) {
            case STORE:
            {
                // Write a store opcode to the program
                program_set_bytecode(user_program, opcode);

                // Get next token, which should be a register
                t += strlen(ops_enum_strings[opcode]);
                check((t = skip_space(t, end)));

                // Convert text to register enum
                check((r = get_register(t, end)) != -1);

                // Write register enum to the program
                program_set_bytecode(user_program, r);

                // Get next token, which should be an int
                t += 2; // Regster string length is 2
                check((t = skip_space(t, end)));
                check((t = skip_comma(t, end)));
                check((t = skip_space(t, end)));
                check((t_end = next_eol(t, end)));
                check(memcpy(buf, t, t_end - t));

                // Convert text to int
                v = strtol(buf, NULL, 0);
                program_set_bytecode(user_program, v);

                p = t_end;
            }
            break;
            case MOVE:
            {
                // Write a move opcode to the program
                program_set_bytecode(user_program, opcode);

                // Get next token, which should be a register
                t += strlen(ops_enum_strings[opcode]);
                check((t = skip_space(t, end)));

                // Convert text to register enum
                check((r = get_register(t, end)) != -1);

                // Write register enum to the program
                program_set_bytecode(user_program, r);

                // Get next token, which should be
                t += 2; // Regster string length is 2

                check((t = skip_space(t, end)));
                check((t = skip_comma(t, end)));
                check((t = skip_space(t, end)));
                check((t_end = next_eol(t, end)));
                check((r = get_register(t, end)) != -1);
                program_set_bytecode(user_program, r);

                p = t_end;

            }
            break;
            case INC:
            case INCEQ:
            case DEC:
            case DECEQ:
            {
                program_set_bytecode(user_program, opcode);

                // Get next token, which should be a register
                t += strlen(ops_enum_strings[opcode]);
                check((t = skip_space(t, end)));
                check((r = get_register(t, end)) != -1);
                t+=2;

                program_set_bytecode(user_program, r);
                check((t_end = next_eol(t, end)));

                p = t_end;
            }
            break;
            case LABEL:
            case JMP:
            case JMPLT:
            case JMPGT:
            case JMPNE:
            case JMPEQ:
            {
                // Write opcode to the program
                program_set_bytecode(user_program, opcode);

                // Get next token, which should be a number
                t += strlen(ops_enum_strings[opcode]);
                check((t = skip_space(t, end)));
                check((t_end = next_eol(t, end)));
                check(memcpy(buf, t, t_end - t));

                v = strtol(buf, NULL, 0);
                program_set_bytecode(user_program, v);

                p = t_end;

                if (opcode == LABEL) {
                    user_program->labels[v] = user_program->bytecode_len - 1;
                }

            }
            break;
            case CMP:
            case ADD:
            case SUB:
            case MUL:
            case RET:
            {
                // Write opcode to the program
                program_set_bytecode(user_program, opcode);
                t += strlen(ops_enum_strings[opcode]);
                p = t;
                break;
            }
            case OPS_COUNT:
            {
                break;
            }
        }
    }

    return;

error:
    exit(100);

}

void fightvm_init()
{
    srand(SDL_GetTicks());
    SDL_Delay(500);
}

static void vertline(int x, int y1, int y2, int c)
{
    for (int i = y1; i < y2; i++) {
        cpu_texture.pixels[i * W + x] = c; 
    }
}

static void draw()
{
    int hp;
    double scale;

    memset(cpu_texture.pixels, 0, W * H * sizeof(Uint32));

    scale = ((double)user_program[0].hp / (double)MAX_HP);
    hp = scale * W;
    for (int i = 0; i < hp; i++) {
       vertline(i, 0, (H / 2) - 1, 0xff000000); 
    }
    scale = ((double)user_program[1].hp / (double)MAX_HP);
    hp = scale * W;
    for (int i = 0; i < hp; i++) {
       vertline(i, H / 2, H - 1, 0x0000ff00); 
    }
}

static void sync()
{
    // Tick
    static Uint32 current_millis;
    static Uint32 last_millis = 0;

    current_millis = SDL_GetTicks();
    if (current_millis % 1000 == 0) {
        //printf("%d\n", current_millis - last_millis);
    }
    while (current_millis < last_millis + 16) {
        current_millis = SDL_GetTicks();
        SDL_Delay(2);
    }
    last_millis = current_millis;

    // Update the gpu texture
    SDL_Rect r = {.w = cpu_texture.w,.h = cpu_texture.h,.x = 0,.y = 0 };
    SDL_UpdateTexture(gpu_texture, &r, cpu_texture.pixels,
		      cpu_texture.w * sizeof(Uint32));
    SDL_RenderCopy(renderer, gpu_texture, NULL, NULL);

    // Do hardware drawing
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
   
    // Present
    SDL_RenderPresent(renderer);

}

#define ipcode (code[ip])
int fightvm_run_program(int program_index)
{
    program *p = &user_program[program_index];
    register int i0 = 0;
    register int i1 = 0;
    int ip = 0;
    register int op = 0;
    int *code = p->bytecode;
    int len = p->bytecode_len;

    memset(registers, 0, sizeof(registers));

    if (program_index == 0) {
        registers[C0] = user_program[0].hp;
        registers[E0] = user_program[1].hp;
    } else {
        registers[C0] = user_program[1].hp;
        registers[E0] = user_program[0].hp;
    }

    while(ip < len && (op = p->bytecode[ip]) != -1) {
        registers[T0] = SDL_GetTicks();
        switch(op){

            case STORE:

                // data from code
                ip++;
                i0 = code[ip];

                // data from code
                ip++;
                i1 = code[ip];

                registers[i0] = i1;
                break;

            case MOVE:
                // data from code
                ip++;
                i0 = code[ip];

                // data from code
                ip++;
                i1 = code[ip];

                registers[i0] = registers[i1];

                break;

            case ADD:
                registers[O0] = registers[I0] + registers[I1];
                break;

            case SUB:
                registers[O0] = registers[I0] - registers[I1];
                break;

            case MUL:
                registers[O0] = registers[I0] * registers[I1];
                break;

            case INC:
                ip++;
                registers[ipcode]++;
                break;

            case DEC:
                ip++;
                registers[ipcode]--;
                break;

            case INCEQ:
                ip++;
                if (flags[EQ]) {
                    registers[ipcode]++;
                }
                break;

            case DECEQ:
                ip++;
                if (flags[EQ]) {
                    registers[ipcode]--;
                }
                break;

            case LABEL:
                ip++;
                break;

            case RET:
                ip = len;
                break;

            case CMP:
                i0 = registers[I0];
                i1 = registers[I1];
                flags[EQ] = i0 == i1;
                flags[LT] = i0 < i1;
                flags[GT] = i0 > i1;
                flags[ER] = 0;
                break;

            case JMPEQ:
                ip++;
                i0 = code[ip];
                if (flags[EQ]) {
                    ip = p->labels[i0];
                }
                break;

            case JMPNE:
                ip++;
                i0 = code[ip];
                if (!flags[EQ]) {
                    ip = p->labels[i0];
                }
                break;

            case JMPGT:
                ip++;
                i0 = code[ip];
                if (flags[GT]) {
                    ip = p->labels[i0];
                }
                break;

            case JMPLT:
                ip++;
                i0 = code[ip];
                if (flags[LT]) {
                    ip = p->labels[i0];
                }
                break;

            case JMP:
                ip++;
                i0 = code[ip];
                ip = p->labels[i0];
                break;
        }
        ip++;
    }
    if (registers[R0] < 0 || registers[R0] > 2) {
        registers[R0] = 0;
    }
    return registers[R0];

}

void fightvm_resolve_round(int results[])
{
    for (int i = 0; i < PROGRAM_COUNT; i++) {
        if (results[i] == Gamble) {
            int gamble_win = randrange(1, 100) > 90;
            results[i] = gamble_win ? Attack : Defend;
            if (gamble_win) {
                user_program[i].strength++;
            }
        }
    }

    program_result_lut *t;
    int n = sizeof(result_table) / sizeof(program_result_lut);
    int damage = 0;
    for (int i = 0; i < n; i++) {
        t = &result_table[i];
        if (t->program_one_intent == results[0] && t->program_two_intent == results[1]) {

            damage = (t->program_one_damage_taken * user_program[1].strength);
            user_program[0].hp -= damage;
            printf("%s takes %d damage.\n", user_program[0].name, damage); 
            
            damage = (t->program_two_damage_taken * user_program[0].strength);
            user_program[1].hp -= damage;
            printf("%s takes %d damage.\n", user_program[1].name, damage); 

        }
    }

    if (user_program[0].hp < 0) { user_program[0].hp = 0; }
    if (user_program[1].hp < 0) { user_program[1].hp = 0; }
}

void fightvm_program_loop()
{
    int rounds = 0;
    int result[PROGRAM_COUNT];
    
    user_program[0].hp = MAX_HP;
    user_program[0].strength = 1;
    user_program[1].hp = MAX_HP;
    user_program[1].strength = 1;


    while (!done() && user_program[0].hp > 0 && user_program[1].hp > 0) {
        rounds++;
        result[0] = fightvm_run_program(0);
        result[1] = fightvm_run_program(1);
        printf("%s has chosen to %s.\n", user_program[0].name,
                program_result_enum_strings_lower[result[0]]);
        printf("%s has chosen to %s.\n", user_program[1].name,
                program_result_enum_strings_lower[result[1]]);

        fightvm_resolve_round(result);

        draw();
        sync();

        puts("--------------------");

    }

    printf("%s has %d hitpoints left after %d rounds.\n", user_program[0].name, user_program[0].hp, rounds);
    printf("%s has %d hitpoints left after %d rounds.\n", user_program[1].name, user_program[1].hp, rounds);

    while (!done()) {
        SDL_Delay(100);
    }

}
    
int main(int argc, char *argv[])
{
    if (argc != 3) return 0;
    const char *code1_path_arg = argv[1];
    const char *code2_path_arg = argv[2];

    // sdl init
    SDL_Init(SDL_INIT_EVERYTHING);

    // no filtering
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    // create window
    SDL_Window *window = SDL_CreateWindow("pixels", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            W * 6, H * 6, SDL_WINDOW_RESIZABLE);

    // create renderer
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

    // create gpu texture
    gpu_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_RenderSetLogicalSize(renderer, W, H);
    SDL_SetRenderTarget(renderer, gpu_texture);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    // create cpu texture
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
#define rmask 0xff000000
#define gmask 0x00ff0000
#define bmask 0x0000ff00
#define amask 0x000000ff
#else
#define rmask 0x000000ff
#define gmask 0x0000ff00
#define bmask 0x00ff0000
#define amask 0xff000000
#endif
    SDL_Surface *surface = SDL_CreateRGBSurface(0, W, H, 32, rmask, gmask, bmask, amask);
    cpu_texture.surface = surface;
    cpu_texture.w = surface->w;
    cpu_texture.h = surface->h;
    cpu_texture.pixels = (Uint32 *) surface->pixels;
    SDL_SetSurfaceBlendMode(cpu_texture.surface, SDL_BLENDMODE_NONE);

    read_code(code1_path_arg, &user_program[0]);
    parse_code(&user_program[0]);

    read_code(code2_path_arg, &user_program[1]);
    parse_code(&user_program[1]);

    fightvm_init();
    #ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(fightvm_program_loop, 0, 0);
    #else
    fightvm_program_loop();
    #endif

    // Cleanup
    SDL_DestroyTexture(gpu_texture);
    SDL_FreeSurface(surface);
    return 0;
}

