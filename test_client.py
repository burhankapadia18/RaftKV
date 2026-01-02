import requests
import msgpack
import time

# Configuration
URL = "http://localhost:8080"
SET_ENDPOINT = f"{URL}/insert-val"
GET_ENDPOINT = f"{URL}/get-val"

def send_msgpack_request(key, value):
    """Sends a SET command using MsgPack binary data."""
    
    # 1. Create the dictionary payload
    payload = {
        "op": "SET",
        "key": key,
        "value": value
    }

    # 2. Serialize to MsgPack binary
    # use_bin_type=True ensures raw bytes are handled correctly
    serialized_data = msgpack.packb(payload, use_bin_type=True)

    # 3. Define Headers
    headers = {
        "Content-Type": "application/msgpack",
        # It's good practice to set Content-Length, though requests does it automatically
        "Content-Length": str(len(serialized_data))
    }

    print(f"Sending MsgPack payload ({len(serialized_data)} bytes)...")
    
    try:
        # 4. Send POST request with raw body
        response = requests.post(
            SET_ENDPOINT, 
            data=serialized_data, 
            headers=headers
        )
        
        print(f"Response Status: {response.status_code}")
        print(f"Response Body: {response.text}")
        
        return response.status_code == 200
        
    except requests.exceptions.RequestException as e:
        print(f"Error sending request: {e}")
        return False

def verify_get(key):
    """Verifies the data using the legacy GET endpoint."""
    try:
        response = requests.get(GET_ENDPOINT, params={"key": key})
        print(f"GET Verification for '{key}': {response.text}")
    except Exception as e:
        print(f"Verification failed: {e}")

if __name__ == "__main__":
    # Test Data
    test_key = "user_123"
    test_value = "msgpack_optimization_active"

    # Run Test
    if send_msgpack_request(test_key, test_value):
    # if True:
        print("\nWaiting for Raft consensus...")
        time.sleep(0.3) # Give a moment for Raft replication
        verify_get(test_key)
    else:
        print("MsgPack request failed.")