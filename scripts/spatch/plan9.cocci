@ print@
@@
-print(
+printd(
...)

@ channel @
identifier d;
@@
-Chan
+struct chan
d;

@ channelstar @
identifier d;
@@
-Chan *
+struct chan *
d;

@mount@
identifier d;
@@
-Mount
+struct mount
d;

@ mountstar @
identifier d;
@@
-Chan *
+struct mount *
d;

@uvlong@
identifier d;
@@
-uvlong
+uint64_t
d;
@vlong@
identifier d;
@@
-vlong
+int64_t
d;
@ulong@
identifier d;
@@
-ulong
+uint32_t
d;
@ushort@
identifier d;
@@
-ushort
+uint16_t
d;

@ rulesm @
identifier t;
identifier f;
expression E1;
type T;
@@
T f(...){<...
t = smalloc(E1);
...>}
@@
identifier rulesm.f;
expression E1;
@@

- smalloc(E1
+ kmalloc(E1, 0
   )

@ rulem @
identifier t;
identifier f;
expression E1;
type T;
@@
T f(...){<...
t = malloc(E1);
...>}
@@
identifier rulem.f;
expression E1;
@@

- malloc(E1
+ kmalloc(E1, 0
   )

@@
@@
-getcallerpc(...);
@@
@@
-setmalloctag(...);

@@
type T;
@@
-T validname0(...){...}

@@
type T;
@@
-T kstrcpy(...){...}

@@
@@
-if (up){
...
-} else {...}

@@
expression E;
@@
-strcpy(up->errstr,
+set_errstr(
E)
@@
@@
-saveregisters(...);
@@
@@
-saveregisters(...){...}
@@
@@
+//
muxclose(...);
