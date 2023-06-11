int count = 0;
int lastValue = HIGH;

void setup() {
  // put your setup code here, to run once:
  pinMode(2, INPUT_PULLUP);
  Serial.begin(9600);
  lastValue = digitalRead(2);
  attachInterrupt(digitalPinToInterrupt(2), Add, CHANGE);
}

void loop() {
  // put your main code here, to run repeatedly:
}


void Add()
{
  int value = digitalRead(2);
  if(lastValue != value )
  {
    if(value == LOW)
    {
      count++;
    }
    lastValue = value;
  }
  char buffer[10];
  sprintf(buffer, "%.2f", count/10);
  Serial.print("Variable 1:");
  Serial.print(value);
  Serial.print(",Variable 2:");
  Serial.print(count);
  Serial.print(",Variable 3:");
  Serial.println(buffer);
}
